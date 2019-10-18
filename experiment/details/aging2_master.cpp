/**
 * Copyright (C) 2019 Dean De Leo, email: dleo[at]cwi.nl
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "aging2_master.hpp"

#include <cassert>
#include <cmath>
#include <fstream>
#include <iostream>
#include <mutex>
#include <thread>
#include <utility>

#include "common/error.hpp"
#include "common/system.hpp"
#include "common/timer.hpp"
#include "experiment/aging2_experiment.hpp"
#include "experiment/aging2_result.hpp"
#include "reader/graphlog_reader.hpp"
#include "library/interface.hpp"
#include "aging2_worker.hpp"
#include "build_thread.hpp"
#include "configuration.hpp"

using namespace common;
using namespace std;

/*****************************************************************************
 *                                                                           *
 * Debug                                                                     *
 *                                                                           *
 *****************************************************************************/
extern mutex _log_mutex [[maybe_unused]];
//#define DEBUG

#define COUT_DEBUG_FORCE(msg) { scoped_lock<mutex> lock(_log_mutex); cout << "[Aging2Master::" << __FUNCTION__ << "] [" << concurrency::get_thread_id() << "] " << msg << endl; }
#if defined(DEBUG)
    #define COUT_DEBUG(msg) COUT_DEBUG_FORCE(msg)
#else
    #define COUT_DEBUG(msg)
#endif

/*****************************************************************************
 *                                                                           *
 * Init                                                                      *
 *                                                                           *
 *****************************************************************************/
namespace experiment::details {

Aging2Master::Aging2Master(const Aging2Experiment& parameters) :
    m_parameters(parameters),
    m_is_directed(m_parameters.m_library->is_directed()),
    m_results(parameters) {

    auto properties = reader::graphlog::parse_properties(parameters.m_path_log);
    m_results.m_num_artificial_vertices = stoull(properties["internal.vertices.temporary.cardinality"]);
    m_results.m_num_vertices_load = stoull(properties["internal.vertices.final.cardinality"]);
    m_results.m_num_edges_load = stoull(properties["internal.edges.final"]);
    m_results.m_num_operations_total = stoull(properties["internal.edges.cardinality"]);

    // 1024 is a hack to avoid issues with small graphs
    m_reported_times = new uint64_t[static_cast<uint64_t>( ::ceil( static_cast<double>(num_operations_total())/num_edges_final_graph())  + 1 )]();

    m_parameters.m_library->on_main_init(m_parameters.m_num_threads + /* this + builder service */ 2);

    init_workers();
    m_parameters.m_library->on_thread_init(m_parameters.m_num_threads);
}

Aging2Master::~Aging2Master(){
    for(auto w: m_workers){ delete w; }
    m_workers.clear();
    m_parameters.m_library->on_thread_destroy(m_parameters.m_num_threads);
    m_parameters.m_library->on_main_destroy();

    delete[] m_reported_times; m_reported_times = nullptr;
}

void Aging2Master::init_workers() {
    Timer timer; timer.start();
    LOG("[Aging2] Initialising " << parameters().m_num_threads << " worker threads ... ");

    m_workers.reserve(parameters().m_num_threads);
    for(uint64_t worker_id = 0; worker_id < parameters().m_num_threads; worker_id++){
        m_workers.push_back ( new Aging2Worker(*this, worker_id) );
    }

    LOG("[Aging2] Workers initialised in " << timer);
}

/*****************************************************************************
 *                                                                           *
 * Experiment                                                                *
 *                                                                           *
 *****************************************************************************/
void Aging2Master::load_edges(){
    LOG("[Aging2] Loading the sequence of updates to perform from " << m_parameters.m_path_log << " ...");
    Timer timer; timer.start();

    fstream handle(m_parameters.m_path_log, ios_base::in | ios_base::binary);
    auto properties = reader::graphlog::parse_properties(handle);
    uint64_t array_sz = stoull(properties["internal.edges.block_size"]);
    unique_ptr<uint64_t[]> ptr_array1 { new uint64_t[array_sz] };
    unique_ptr<uint64_t[]> ptr_array2 { new uint64_t[array_sz] };
    uint64_t* array1 = ptr_array1.get();
    uint64_t* array2 = ptr_array2.get();
    reader::graphlog::set_marker(properties, handle, reader::graphlog::Section::EDGES);

    reader::graphlog::EdgeLoader loader(handle);
    uint64_t num_edges = loader.load(array1, array_sz / 3);
    while( num_edges > 0 ){
        // partition the batch among the workers
        for(auto w: m_workers) w->load_edges(array1, num_edges);
        if(m_results.m_random_vertex_id == 0) { set_random_vertex_id(array1, num_edges); }

        // load the next batch in the meanwhile
        num_edges = loader.load(array2, array_sz /3);

        // wait for the workers to complete
        for(auto w: m_workers) w->wait();

        swap(array1,array2);
    }
    handle.close();

    timer.stop();
    LOG("[Aging2] Graphlog loaded in " << timer);
}


void Aging2Master::do_run_experiment(){
    LOG("[Aging2] Experiment started ...");
    m_last_progress_reported = 0;
    m_last_time_reported = 0; m_time_start = chrono::steady_clock::now();

    // init the build service (the one that creates the new snapshots/deltas)
    BuildThread build_service { parameters().m_library , static_cast<int>(parameters().m_num_threads) +1, parameters().m_build_frequency };

    Timer timer; timer.start();
    for(auto w: m_workers) w->execute_updates();
    for(auto w: m_workers) w->wait();
    build_service.stop();
    m_parameters.m_library->build(); // flush last changes
    timer.stop();
    LOG("[Aging2] Experiment completed!");
    LOG("[Aging2] Updates performed with " << parameters().m_num_threads << " threads in " << timer);
    m_results.m_completion_time = timer.microseconds();
    m_results.m_num_build_invocations = build_service.num_invocations();
}

void Aging2Master::remove_vertices(){
    LOG("[Aging2] Removing the list of temporary vertices ...");
    Timer timer; timer.start();

    fstream handle(m_parameters.m_path_log, ios_base::in | ios_base::binary);
    auto properties = reader::graphlog::parse_properties(handle);
    uint64_t num_vertices = stoull(properties["internal.vertices.temporary.cardinality"]);
    unique_ptr<uint64_t[]> ptr_vertices { new uint64_t[num_vertices] };
    uint64_t* vertices = ptr_vertices.get();
    reader::graphlog::set_marker(properties, handle, reader::graphlog::Section::VTX_TEMP);

    reader::graphlog::VertexLoader loader { handle };
    loader.load(vertices, num_vertices);
    m_results.m_num_artificial_vertices = num_vertices;

    for(auto w: m_workers) w->remove_vertices(vertices, num_vertices);
    for(auto w: m_workers) w->wait();
    m_parameters.m_library->build();


    LOG("[Aging2] Number of extra vertices: " << m_results.m_num_artificial_vertices << ", "
            "expansion factor: " << static_cast<double>(m_results.m_num_artificial_vertices +  m_results.m_num_vertices_final_graph) / m_results.m_num_vertices_final_graph);
    timer.stop();
    LOG("[Aging2] Temporary vertices removed in " << timer);
}

Aging2Result Aging2Master::execute(){
    load_edges();
    do_run_experiment();
    remove_vertices();

    store_results();
    log_num_vtx_edges();

    return m_results;
}

/*****************************************************************************
 *                                                                           *
 * Utility methods                                                           *
 *                                                                           *
 *****************************************************************************/
uint64_t Aging2Master::num_operations_total() const {
    return m_results.m_num_operations_total;
}

uint64_t Aging2Master::num_edges_final_graph() const {
   return m_results.m_num_edges_load;
}

void Aging2Master::store_results(){
    m_results.m_num_vertices_final_graph = parameters().m_library->num_vertices();
    m_results.m_num_edges_final_graph = parameters().m_library->num_edges();
    m_results.m_reported_times.reserve(m_last_time_reported);
    for(size_t i = 0, sz = m_last_time_reported; i < sz; i++){
        m_results.m_reported_times.push_back( m_reported_times[i] );
    }
}

void Aging2Master::log_num_vtx_edges(){
    scoped_lock<mutex> lock(_log_mutex);
    cout << "[Aging2] Number of stored vertices: " << m_results.m_num_vertices_final_graph << " [match: ";
    if(m_results.m_num_vertices_load == m_results.m_num_vertices_final_graph){ cout << "yes"; } else {
        cout << "no, expected " << m_results.m_num_vertices_load;
    }
    cout << "], number of stored edges: " << m_results.m_num_edges_final_graph << " [match: ";
    if(m_results.m_num_edges_load == m_results.m_num_edges_final_graph){ cout << "yes"; } else {
        cout << "no, expected " << m_results.m_num_edges_load;
    }
    cout << "]" << endl;
}

void Aging2Master::set_random_vertex_id(uint64_t* edges, uint64_t num_edges){
    uint64_t* __restrict sources = edges;
    uint64_t* __restrict destinations = sources + num_edges;
    double* __restrict weights = reinterpret_cast<double*>(destinations + num_edges);

    uint64_t i = 0;
    while(i < num_edges && weights[i] <= 0) i++;

    if(i < num_edges)
        m_results.m_random_vertex_id = sources[i];
}

} // namespace