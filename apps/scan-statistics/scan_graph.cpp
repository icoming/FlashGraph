/**
 * Copyright 2013 Da Zheng
 *
 * This file is part of SA-GraphLib.
 *
 * SA-GraphLib is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * SA-GraphLib is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with SA-GraphLib.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <signal.h>
#include <google/profiler.h>

#include <set>
#include <vector>

#include "thread.h"
#include "io_interface.h"

#include "graph_engine.h"
#include "graph_config.h"

const double BIN_SEARCH_RATIO = 10;

atomic_number<long> num_working_vertices;
atomic_number<long> num_completed_vertices;

int timestamp;
int timestamp_range;

class count_msg: public vertex_message
{
	int num;
public:
	count_msg(int num) {
		this->num = num;
	}

	int get_num() const {
		return num;
	}
};

class scan_vertex: public compute_vertex
{
	// The number of vertices that have joined with the vertex.
	int num_joined;
	std::vector<vertex_id_t>::const_iterator fetch_it;
	// The number of edges in its neighborhood in different timestamps.
	std::vector<atomic_integer> *num_edges;
	// The number of edges of the vertex in different timestamps.
	std::vector<int> *num_local_edges;
	// All neighbors (in both in-edges and out-edges)
	// in the specified timestamp.
	std::vector<vertex_id_t> *neighbors;

	// The final result.
	double result;
public:
	scan_vertex(): compute_vertex(-1, -1, 0) {
		num_joined = 0;
		num_edges = NULL;
		num_local_edges = NULL;
		neighbors = NULL;
	}

	scan_vertex(vertex_id_t id, off_t off, int size): compute_vertex(
			id, off, size) {
		num_joined = 0;
		num_edges = NULL;
		num_local_edges = NULL;
		neighbors = NULL;
	}

	double get_result() const {
		return result;
	}

	virtual bool has_required_vertices() const {
		if (neighbors == NULL)
			return false;
		return fetch_it != neighbors->end();
	}

	virtual vertex_id_t get_next_required_vertex() {
		vertex_id_t id = *fetch_it;
		fetch_it++;
		return id;
	}

	int count_edges(const TS_page_vertex *v,
			const std::vector<vertex_id_t> *neighbors, int timestamp);
	int count_edges(const TS_page_vertex *v,
			const std::vector<vertex_id_t> *neighbors, int timestamp,
			edge_type type);

	bool run(graph_engine &graph, const page_vertex *vertex);

	bool run_on_neighbors(graph_engine &graph,
			const page_vertex *vertices[], int num);

	void run_on_messages(graph_engine &graph,
			const vertex_message *msgs[], int num) {
	}
};

int scan_vertex::count_edges(const TS_page_vertex *v1,
		const std::vector<vertex_id_t> *neighbors, int timestamp, edge_type type)
{
	const TS_page_directed_vertex *v = (const TS_page_directed_vertex *) v1;
	int num_local_edges = 0;
	int num_v_edges = v->get_num_edges(timestamp, type);
	if (num_v_edges == 0)
		return 0;

	page_byte_array::const_iterator<vertex_id_t> other_it
		= v->get_neigh_begin(timestamp, type);
	page_byte_array::const_iterator<edge_count> other_data_it
		= v->get_edge_data_begin<edge_count>(timestamp, type);
	page_byte_array::const_iterator<vertex_id_t> other_end
		= v->get_neigh_end(timestamp, type);

	std::vector<vertex_id_t>::const_iterator this_it = neighbors->begin();
	std::vector<vertex_id_t>::const_iterator this_end = neighbors->end();

	if (num_v_edges / neighbors->size() > BIN_SEARCH_RATIO) {
		for (; this_it != neighbors->end(); this_it++) {
			vertex_id_t this_neighbor = *this_it;
			// We need to skip loops.
			if (this_neighbor == v->get_id()
					|| this_neighbor == this->get_id()) {
				continue;
			}

			page_byte_array::const_iterator<vertex_id_t> first
				= std::lower_bound(other_it, other_end, this_neighbor);
			// found it.
			if (first != other_end && !(this_neighbor < *first)) {
				do {
					page_byte_array::const_iterator<edge_count> data_it
						= other_data_it;
					data_it += first - other_it;
					// Edges in the v's neighbor lists may duplicated.
					// The duplicated neighbors need to be counted
					// multiple times.
					num_local_edges += (*data_it).get_count();
					++first;
					++data_it;
				} while (first != other_end && this_neighbor == *first);
			}
		}
	}
	else if (neighbors->size() / num_v_edges > BIN_SEARCH_RATIO) {
		while (other_it != other_end) {
			vertex_id_t neigh_neighbor = *other_it;
			if (neigh_neighbor != v->get_id()
					&& neigh_neighbor != this->get_id()) {
				if (std::binary_search(neighbors->begin(), neighbors->end(),
							neigh_neighbor)) {
					num_local_edges += (*other_data_it).get_count();
				}
			}
			++other_it;
			++other_data_it;
		}
	}
	else {
		while (other_it != other_end && this_it != this_end) {
			vertex_id_t this_neighbor = *this_it;
			vertex_id_t neigh_neighbor = *other_it;
			if (neigh_neighbor == v->get_id()
					|| neigh_neighbor == this->get_id()) {
				++other_it;
				++other_data_it;
				continue;
			}
			if (this_neighbor == neigh_neighbor) {
				do {
					// Edges in the v's neighbor lists may duplicated.
					// The duplicated neighbors need to be counted
					// multiple times.
					num_local_edges += (*other_data_it).get_count();
					++other_it;
					++other_data_it;
				} while (other_it != other_end && this_neighbor == *other_it);
				++this_it;
			}
			else if (this_neighbor < neigh_neighbor) {
				++this_it;
			}
			else {
				++other_it;
				++other_data_it;
			}
		}
	}
	return num_local_edges;
}

int scan_vertex::count_edges(const TS_page_vertex *v,
		const std::vector<vertex_id_t> *neighbors, int timestamp)
{
	if (v->get_num_edges(timestamp, edge_type::BOTH_EDGES) == 0
			|| neighbors->empty())
		return 0;

	return count_edges(v, neighbors, timestamp, edge_type::IN_EDGE)
		+ count_edges(v, neighbors, timestamp, edge_type::OUT_EDGE);
}

template<class InputIterator1, class InputIterator2, class Skipper,
	class OutputIterator>
int unique_merge(InputIterator1 it1, InputIterator1 last1,
		InputIterator2 it2, InputIterator2 last2, Skipper skip,
		OutputIterator result)
{
	OutputIterator result_begin = result;
	while (it1 != last1 && it2 != last2) {
		if (*it1 > *it2) {
			typename std::iterator_traits<InputIterator2>::value_type v = *it2;
			if (!skip(v))
				*(result++) = v;
			while (*it2 == v && it2 != last2)
				++it2;
		}
		else if (*it1 < *it2) {
			typename std::iterator_traits<InputIterator1>::value_type v = *it1;
			if (!skip(v))
				*(result++) = v;
			while (*it1 == v && it1 != last1)
				++it1;
		}
		else {
			typename std::iterator_traits<InputIterator1>::value_type v = *it1;
			if (!skip(v))
				*(result++) = v;
			while (*it1 == v && it1 != last1)
				++it1;
			while (*it2 == v && it2 != last2)
				++it2;
		}
	}

	while (it1 != last1) {
		typename std::iterator_traits<InputIterator1>::value_type v = *it1;
		if (!skip(v))
			*(result++) = v;
		while (*it1 == v && it1 != last1)
			++it1;
	}

	while (it2 != last2) {
		typename std::iterator_traits<InputIterator2>::value_type v = *it2;
		if (!skip(v))
			*(result++) = v;
		while (*it2 == v && it2 != last2)
			++it2;
	}
	return result - result_begin;
}

bool scan_vertex::run(graph_engine &graph, const page_vertex *vertex)
{
	assert(neighbors == NULL);
	assert(num_joined == 0);

	const TS_page_directed_vertex *ts_vertex
		= (const TS_page_directed_vertex *) vertex;
	long ret = num_working_vertices.inc(1);
	if (ret % 100000 == 0)
		printf("%ld working vertices\n", ret);
	if (ts_vertex->get_num_edges(timestamp, edge_type::BOTH_EDGES) == 0) {
		long ret = num_completed_vertices.inc(1);
		if (ret % 100000 == 0)
			printf("%ld completed vertices\n", ret);
		return true;
	}

	num_edges = new std::vector<atomic_integer>(timestamp_range);
	num_local_edges = new std::vector<int>(timestamp_range);
	neighbors = new std::vector<vertex_id_t>(ts_vertex->get_num_edges(
				timestamp, edge_type::BOTH_EDGES));

	class skip_self {
		vertex_id_t id;
	public:
		skip_self(vertex_id_t id) {
			this->id = id;
		}

		bool operator()(vertex_id_t id) {
			return this->id == id;
		}
	};

	int num_neighbors = unique_merge(
			ts_vertex->get_neigh_begin(timestamp, edge_type::IN_EDGE),
			ts_vertex->get_neigh_end(timestamp, edge_type::IN_EDGE),
			ts_vertex->get_neigh_begin(timestamp, edge_type::OUT_EDGE),
			ts_vertex->get_neigh_end(timestamp, edge_type::OUT_EDGE),
			skip_self(ts_vertex->get_id()),
			neighbors->begin());
	neighbors->resize(num_neighbors);

	page_byte_array::const_iterator<vertex_id_t> it = ts_vertex->get_neigh_begin(
			timestamp, edge_type::BOTH_EDGES);
	page_byte_array::const_iterator<edge_count> data_it
		= ts_vertex->get_edge_data_begin<edge_count>(timestamp,
				edge_type::BOTH_EDGES);
	page_byte_array::const_iterator<vertex_id_t> end = ts_vertex->get_neigh_end(
			timestamp, edge_type::BOTH_EDGES);
	for (; it != end; ++it, ++data_it) {
		vertex_id_t id = *it;
		// Ignore loops
		if (id != ts_vertex->get_id()) {
			num_local_edges->at(0) += (*data_it).get_count();
		}
	}

	if (neighbors->size() == 0) {
		delete num_edges;
		delete num_local_edges;
		delete neighbors;
		num_edges = NULL;
		num_local_edges = NULL;
		neighbors = NULL;
		long ret = num_completed_vertices.inc(1);
		if (ret % 100000 == 0)
			printf("%ld completed vertices\n", ret);
		return true;
	}

	for (int i = 1; i < timestamp_range && timestamp - i >= 0; i++) {
		int timestamp2 = timestamp - i;
		page_byte_array::const_iterator<vertex_id_t> it
			= ts_vertex->get_neigh_begin(timestamp2, edge_type::BOTH_EDGES);
		page_byte_array::const_iterator<edge_count> data_it
			= ts_vertex->get_edge_data_begin<edge_count>(timestamp2,
				edge_type::BOTH_EDGES);
		page_byte_array::const_iterator<vertex_id_t> end
			= ts_vertex->get_neigh_end(timestamp2, edge_type::BOTH_EDGES);
		for (; it != end; ++it, ++data_it) {
			vertex_id_t id = *it;
			// Ignore loop
			if (id != ts_vertex->get_id()
					// The neighbor needs to exist in timestamp 1.
					&& std::binary_search(neighbors->begin(),
						neighbors->end(), id)) {
				num_local_edges->at(i) += (*data_it).get_count();
			}
		}
	}

	fetch_it = neighbors->begin();
	return false;
}

bool scan_vertex::run_on_neighbors(graph_engine &graph,
		const page_vertex *vertices[], int num)
{
	num_joined++;
	assert(neighbors);
	for (int i = 0; i < num; i++) {
		for (int j = 0; j < timestamp_range && timestamp - j >= 0; j++) {
			int ret = count_edges((const TS_page_vertex *) vertices[i],
					neighbors, timestamp - j);
			if (ret > 0)
				num_edges->at(j).inc(ret);
		}
	}

	// If we have seen all required neighbors, we have complete
	// the computation. We can release the memory now.
	if (num_joined == (int) neighbors->size()) {
		long ret = num_completed_vertices.inc(1);
		if (ret % 100000 == 0)
			printf("%ld completed vertices\n", ret);

		for (int i = 0; i < timestamp_range; i++) {
			assert(num_edges->at(i).get() % 2 == 0);
			num_edges->at(i) = num_edges->at(i).get() / 2
				+ num_local_edges->at(i);
		}

		double avg = 0;
		if (timestamp_range - 1 > 0) {
			double sum = 0;
			for (int i = 1; i < timestamp_range; i++)
				sum += num_edges->at(i).get();
			avg = sum / (timestamp_range - 1);
		}

		double deviation;
		if (timestamp_range - 1 <= 1)
			deviation = 1;
		else {
			double sum = 0;
			for (int i = 1; i < timestamp_range; i++) {
				sum += (num_edges->at(i).get()
						- avg) * (num_edges->at(i).get() - avg);
			}
			sum = sum / (timestamp_range - 2);
			deviation = sqrt(sum);
			if (deviation < 1)
				deviation = 1;
		}
		result = (num_edges->at(0).get() - avg) / deviation;

		delete num_local_edges;
		delete num_edges;
		delete neighbors;
		num_local_edges = NULL;
		num_edges = NULL;
		neighbors = NULL;
		return true;
	}
	return false;
}

void int_handler(int sig_num)
{
	if (!graph_conf.get_prof_file().empty())
		ProfilerStop();
	exit(0);
}

int main(int argc, char *argv[])
{
	if (argc < 8) {
		fprintf(stderr,
				"scan-statistics conf_file graph_file index_file directed num_timestamps timestamp timestamp_range [output_file]\n");
		graph_conf.print_help();
		params.print_help();
		exit(-1);
	}

	std::string conf_file = argv[1];
	std::string graph_file = argv[2];
	std::string index_file = argv[3];
	bool directed = atoi(argv[4]);
	int num_timestamps = atoi(argv[5]);
	timestamp = atoi(argv[6]);
	timestamp_range = atoi(argv[7]);
	assert(directed);
	std::string output_file;
	if (argc == 9) {
		output_file = argv[8];
		argc--;
	}

	config_map configs(conf_file);
	configs.add_options(argv + 8, argc - 8);
	graph_conf.init(configs);
	graph_conf.print();

	signal(SIGINT, int_handler);
	init_io_system(configs);

	graph_index *index = graph_index_impl<scan_vertex>::create(
			index_file, sizeof(ts_ext_mem_directed_vertex));
	graph_engine *graph = graph_engine::create(
			graph_conf.get_num_threads(), params.get_num_nodes(), graph_file,
			index, new ts_ext_mem_vertex_interpreter(num_timestamps), directed);
	// TODO I need to redefine this interface.
	graph->set_required_neighbor_type(edge_type::BOTH_EDGES);
	printf("scan statistics starts\n");
	printf("prof_file: %s\n", graph_conf.get_prof_file().c_str());
	if (!graph_conf.get_prof_file().empty())
		ProfilerStart(graph_conf.get_prof_file().c_str());

	struct timeval start, end;
	gettimeofday(&start, NULL);
	graph->start_all();
	graph->wait4complete();
	gettimeofday(&end, NULL);

	if (!graph_conf.get_prof_file().empty())
		ProfilerStop();
	if (graph_conf.get_print_io_stat())
		print_io_thread_stat();
	graph->cleanup();
	printf("It takes %f seconds\n", time_diff(start, end));
	printf("There are %ld vertices\n", index->get_num_vertices());
	printf("process %ld vertices and complete %ld vertices\n",
			num_working_vertices.get(), num_completed_vertices.get());

	double max_res = LONG_MIN;
	vertex_id_t max_v;
	std::vector<vertex_id_t> vertices;
	index->get_all_vertices(vertices);
	for (size_t i = 0; i < index->get_num_vertices(); i++) {
		scan_vertex &v = (scan_vertex &) index->get_vertex(vertices[i]);
		if (max_res < v.get_result()) {
			max_v = v.get_id();
			max_res = v.get_result();
		}
	}
	printf("max value is on v%ld: %f\n", max_v, max_res);

	if (!output_file.empty()) {
		FILE *f = fopen(output_file.c_str(), "w");
		if (f == NULL) {
			perror("fopen");
			return -1;
		}
		std::vector<vertex_id_t> vertices;
		index->get_all_vertices(vertices);
		for (size_t i = 0; i < index->get_num_vertices(); i++) {
			scan_vertex &v = (scan_vertex &) index->get_vertex(vertices[i]);
			fprintf(f, "\"%ld\" %f\n", v.get_id(), v.get_result());
		}
		fclose(f);
	}
}
