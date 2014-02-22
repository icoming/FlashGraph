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

#include <algorithm>
#ifndef MEMCHECK
#include <parallel/algorithm>
#endif

#include "io_interface.h"

#include "bitmap.h"
#include "graph_config.h"
#include "graph_engine.h"
#include "messaging.h"

const int MAX_STOLEN_VERTICES = 1024;

graph_config graph_conf;

class worker_thread;

/**
 * This I/O scheduler is to favor maximizing throughput.
 * Therefore, it processes all user tasks together to potentially increase
 * the page cache hit rate.
 */
class throughput_comp_io_scheduler: public comp_io_scheduler
{
	fifo_queue<io_request> req_buf;
public:
	throughput_comp_io_scheduler(int node_id): comp_io_scheduler(
			node_id), req_buf(node_id, 512, true) {
	}

	size_t get_requests(fifo_queue<io_request> &reqs);
};

class throughput_comp_io_sched_creater: public comp_io_sched_creater
{
public:
	comp_io_scheduler *create(int node_id) const {
		return new throughput_comp_io_scheduler(node_id);
	}
};

struct prio_compute
{
	user_compute *compute;
	io_request req;

	prio_compute(io_interface *io, user_compute *compute) {
		this->compute = compute;
		bool ret = compute->fetch_request(io, req);
		assert(ret);
	}
};

class comp_prio_compute
{
public:
	bool operator()(const prio_compute &c1, const prio_compute &c2) {
		// We want the priority queue returns requests with
		// the smallest offset first. If we use less, the priority queue
		// will return the request with the greatest offset.
		return c1.req.get_offset() > c2.req.get_offset();
	}
};

size_t throughput_comp_io_scheduler::get_requests(fifo_queue<io_request> &reqs)
{
	size_t num = 0;
	// Let's add the buffered requests first.
	if (!req_buf.is_empty()) {
		num = reqs.add(&req_buf);
	}

	if (!reqs.is_full()) {
		// Construct a priority queue on user tasks, ordered by the offset
		// of their next requests.
		std::priority_queue<prio_compute, std::vector<prio_compute>,
			comp_prio_compute> user_computes;
		compute_iterator it = this->get_begin();
		compute_iterator end = this->get_end();
		for (; it != end; ++it) {
			user_compute *compute = *it;
			// Skip the ones without user tasks.
			if (!compute->has_requests())
				continue;

			prio_compute prio_comp(get_io(), compute);
			user_computes.push(prio_comp);
		}

		// Add requests to the queue in a sorted order.
		off_t prev = 0;
		int num = 0;
		while (!reqs.is_full() && !user_computes.empty()) {
			num++;
			prio_compute prio_comp = user_computes.top();
			assert(prev <= prio_comp.req.get_offset());
			prev = prio_comp.req.get_offset();
			user_computes.pop();
			assert(prev <= user_computes.top().req.get_offset());
			reqs.push_back(prio_comp.req);
			num++;
			user_compute *compute = prio_comp.compute;
			if (compute->has_requests()) {
				prio_compute prio_comp(get_io(), compute);
				assert(prio_comp.req.get_offset() >= prev);
				user_computes.push(prio_comp);
			}
		}

		// We have got a request from each user task. We can't add them to
		// the queue this time. We need to buffer them.
		while (!user_computes.empty()) {
			prio_compute prio_comp = user_computes.top();
			user_computes.pop();
			if (req_buf.is_full())
				req_buf.expand_queue(req_buf.get_size() * 2);
			req_buf.push_back(prio_comp.req);
		}
	}
	return num;
}

class default_vertex_scheduler: public vertex_scheduler
{
public:
	void schedule(std::vector<vertex_id_t> &vertices) {
		std::sort(vertices.begin(), vertices.end());
	}
} default_scheduler;

class sorted_vertex_queue
{
	pthread_spinlock_t lock;
	std::vector<vertex_id_t> sorted_vertices;
	size_t fetch_idx;
	vertex_scheduler *scheduler;
public:
	sorted_vertex_queue() {
		fetch_idx = 0;
		pthread_spin_init(&lock, PTHREAD_PROCESS_PRIVATE);
		this->scheduler = &default_scheduler;
	}

	void set_vertex_scheduler(vertex_scheduler *scheduler) {
		this->scheduler = scheduler;
	}

	void init(vertex_id_t buf[], int size, bool sorted) {
		pthread_spin_lock(&lock);
		fetch_idx = 0;
		sorted_vertices.clear();
		sorted_vertices.assign(buf, buf + size);
		if (!sorted)
			scheduler->schedule(sorted_vertices);
		pthread_spin_unlock(&lock);
	}

	void init(const std::vector<vertex_id_t> &vec, bool sorted) {
		pthread_spin_lock(&lock);
		fetch_idx = 0;
		sorted_vertices.clear();
		sorted_vertices.assign(vec.begin(), vec.end());
		if (!sorted)
			scheduler->schedule(sorted_vertices);
		pthread_spin_unlock(&lock);
	}

	void init(const bitmap &map, int part_id,
			const vertex_partitioner *partitioner) {
		pthread_spin_lock(&lock);
		fetch_idx = 0;
		sorted_vertices.clear();
		map.get_set_bits(sorted_vertices);
		// the bitmap only contains the locations of vertices in the bitmap.
		// We have to translate them back to vertex ids.
		for (size_t i = 0; i < sorted_vertices.size(); i++) {
			vertex_id_t id;
			partitioner->loc2map(part_id, sorted_vertices[i], id);
			sorted_vertices[i] = id;
		}

		if (scheduler != &default_scheduler)
			scheduler->schedule(sorted_vertices);
		pthread_spin_unlock(&lock);
	}

	int fetch(vertex_id_t vertices[], int num) {
		pthread_spin_lock(&lock);
		int num_fetches = min(num, sorted_vertices.size() - fetch_idx);
		memcpy(vertices, sorted_vertices.data() + fetch_idx,
				num_fetches * sizeof(vertex_id_t));
		fetch_idx += num_fetches;
		pthread_spin_unlock(&lock);
		return num_fetches;
	}

	bool is_empty() {
		pthread_spin_lock(&lock);
		bool ret = sorted_vertices.size() - fetch_idx == 0;
		pthread_spin_unlock(&lock);
		return ret;
	}

	size_t get_num_vertices() {
		pthread_spin_lock(&lock);
		size_t num = sorted_vertices.size() - fetch_idx;
		pthread_spin_unlock(&lock);
		return num;
	}
};

request_range compute_vertex::get_next_request(graph_engine *graph)
{
	vertex_id_t id = get_next_required_vertex();
	compute_vertex &info = graph->get_vertex(id);
	data_loc_t loc(graph->get_file_id(), info.get_ext_mem_off());
	return request_range(loc, info.get_ext_mem_size(), READ, NULL);
}

void ts_vertex_request::set_vertex(vertex_id_t id)
{
	this->id = id;
	compute_vertex &info = graph->get_vertex(id);
	// There is some overhead to fetch part of a vertex, so we should
	// minize the number of vertices fetched partially.
	// If a vertex is small enough (stored on <= 3 pages), we fetch the entire
	// vertex.
	off_t start_pg = ROUND_PAGE(info.get_ext_mem_off());
	off_t end_pg = ROUNDUP_PAGE(info.get_ext_mem_off() + info.get_ext_mem_size());
	if (end_pg - start_pg <= PAGE_SIZE * 3)
		require_all = true;
}

/**
 * This callback is to process a vertex.
 */
class vertex_compute: public user_compute
{
	// The number of requested vertices that will be read in the user compute.
	int num_complete_issues;
	// The number of vertices read by the user compute.
	int num_complete_fetched;

	graph_engine *graph;
	compute_vertex *v;
	// The thread that creates the vertex compute.
	worker_thread *issue_thread;
public:
	vertex_compute(graph_engine *graph,
			compute_allocator *alloc): user_compute(alloc) {
		this->graph = graph;
		v = NULL;
		issue_thread = (worker_thread *) thread::get_curr_thread();
		num_complete_issues = 0;
		num_complete_fetched = 0;
	}

	virtual int serialize(char *buf, int size) const {
		return 0;
	}

	virtual int get_serialized_size() const {
		return 0;
	}

	virtual int has_requests() const {
		if (v == NULL)
			return false;
		else
			return v->has_required_vertices();
	}

	virtual request_range get_next_request() {
		assert(v);
		request_range range = v->get_next_request(graph);
		if (range.get_compute() == NULL) {
			num_complete_issues++;
			range.set_compute(this);
		}
		return range;
	}

	virtual void run(page_byte_array &);

	virtual bool has_completed() const {
		// If the user compute has got all requested data and it has
		// no more requests to issue, we can consider the user compute
		// has been completed.
		// NOTE: it's possible that requested data may not be passed to
		// this user compute, so we only count the requests that are going
		// to be passed to this user compute.
		return num_complete_issues == num_complete_fetched && !has_requests();
	}
};

/**
 * Sometimes a vertex only needs to read part of its neighbors.
 * This class is to read part of a neighbor and pass the neighbor to
 * the specified vertex to perform computation.
 * An instance of the class only reads one neighbor.
 */
class part_ts_vertex_compute: public user_compute
{
	graph_engine *graph;
	// The vertex where computation should perform.
	compute_vertex *comp_v;
	const TS_page_vertex *required_vertex_header;
	// The part of the vertex will be read and passed to
	// the computation vertex.
	ts_vertex_request required_part;
	// The thread that creates the vertex compute.
	worker_thread *issue_thread;
	int num_issued;
	int num_fetched;
public:
	part_ts_vertex_compute(graph_engine *graph,
			compute_allocator *alloc): user_compute(alloc), required_part(graph) {
		this->graph = graph;
		comp_v = NULL;
		issue_thread = (worker_thread *) thread::get_curr_thread();
		required_vertex_header = NULL;
		num_issued = 0;
		num_fetched = 0;
	}

	void init(compute_vertex *v, const ts_vertex_request &req) {
		comp_v = v;
		required_part = req;
	}

	virtual int serialize(char *buf, int size) const {
		return 0;
	}

	virtual int get_serialized_size() const {
		return 0;
	}

	virtual int has_requests() const {
		return num_issued == 0;
	}

	virtual request_range get_next_request();

	virtual void run(page_byte_array &);

	virtual bool has_completed() const {
		return num_fetched > 0;
	}
};

template<class compute_type>
class vertex_compute_allocator: public compute_allocator
{
	class compute_initiator: public obj_initiator<compute_type>
	{
		graph_engine *graph;
		vertex_compute_allocator<compute_type> *alloc;
	public:
		compute_initiator(graph_engine *graph,
				vertex_compute_allocator<compute_type> *alloc) {
			this->graph = graph;
			this->alloc = alloc;
		}

		virtual void init(compute_type *obj) {
			new (obj) compute_type(graph, alloc);
		}
	};

	obj_allocator<compute_type> allocator;
public:
	vertex_compute_allocator(graph_engine *graph, thread *t): allocator(
			"vertex-compute-allocator", t->get_node_id(), 1024 * 1024,
			params.get_max_obj_alloc_size(),
			// TODO memory leak here
			new compute_initiator(graph, this)) {
	}

	virtual user_compute *alloc() {
		return allocator.alloc_obj();
	}

	virtual void free(user_compute *obj) {
		allocator.free((compute_type *) obj);
	}
};

class worker_thread: public thread
{
	int worker_id;
	file_io_factory *factory;
	io_interface *io;
	graph_engine *graph;
	compute_allocator *alloc;
	compute_allocator *part_alloc;

	/**
	 * A vertex is allowed to send messages to other vertices.
	 * The message passing scheme between vertices are implemented as follows:
	 * all non-empty vertices (with edges) have a message holder;
	 * vertices are partitioned and each worker thread is responsible for
	 * a certin number of vertices;
	 * when a vertex issues messages, the thread that processes the vertex will
	 * redirect them to the right threads;
	 * a thread that receives messages process them and place them in
	 * the right message holder.
	 */

	// The queue of messages sent from other threads.
	msg_queue msg_q;
	slab_allocator *msg_alloc;
	// The message senders to send messages to all other threads.
	// There are n senders, n is the total number of threads used by
	// the graph engine.
	std::vector<simple_msg_sender *> msg_senders;
	std::vector<multicast_msg_sender *> multicast_senders;
	std::vector<multicast_msg_sender *> activate_senders;
	// This is to collect vertices activated in the next level.
	bitmap next_activated_vertices;
	// This contains the vertices activated in the current level.
	sorted_vertex_queue curr_activated_vertices;
	// The thread where we should steal activated vertices from.
	int steal_thread_id;

	// Indicate that we need to start all vertices.
	bool start_all;

	// The number of activated vertices processed in the current level.
	atomic_number<long> num_activated_vertices_in_level;
	// The number of vertices completed in the current level.
	atomic_number<long> num_completed_vertices_in_level;
public:
	worker_thread(graph_engine *graph, file_io_factory *factory, int node_id,
			int worker_id, int num_threads);

	~worker_thread();

	void init_messaging(const std::vector<worker_thread *> &threads);

	void run();
	void init() {
		io = factory->create_io(this);
		io->init();

		// If a user wants to start all vertices.
		if (start_all) {
			std::vector<vertex_id_t> local_ids;
			graph->get_partitioner()->get_all_vertices_in_part(worker_id,
					graph->get_num_vertices(), local_ids);
			assert(curr_activated_vertices.is_empty());
			curr_activated_vertices.init(local_ids, false);
		}
	}

	compute_allocator *get_part_compute_allocator() const {
		return part_alloc;
	}

	multicast_msg_sender *get_activate_sender(int thread_id) const {
		return activate_senders[thread_id];
	}

	multicast_msg_sender *get_multicast_sender(int thread_id) const {
		return multicast_senders[thread_id];
	}

	simple_msg_sender *get_msg_sender(int thread_id) const {
		return msg_senders[thread_id];
	}

	int process_activated_vertices(int max);

	void complete_vertex(const compute_vertex &v) {
		num_completed_vertices_in_level.inc(1);
	}

	void flush_msgs() {
		for (size_t i = 0; i < msg_senders.size(); i++)
			msg_senders[i]->flush();
		for (size_t i = 0; i < multicast_senders.size(); i++)
			multicast_senders[i]->flush();
		for (size_t i = 0; i < activate_senders.size(); i++)
			activate_senders[i]->flush();
	}

	void process_msgs();
	void process_msg(message &msg);
	void process_multicast_msg(multicast_message &mmsg);
	int enter_next_level();

	int steal_activated_vertices(vertex_id_t buf[], int num);

	void start_vertices(const std::vector<vertex_id_t> &vertices) {
		assert(curr_activated_vertices.is_empty());
		curr_activated_vertices.init(vertices, false);
	}

	void start_all_vertices() {
		start_all = true;
	}

	void set_vertex_scheduler(vertex_scheduler *scheduler) {
		curr_activated_vertices.set_vertex_scheduler(scheduler);
	}

	/**
	 * Get the number of vertices being processed in the current level.
	 */
	int get_num_vertices_processing() const {
		return num_activated_vertices_in_level.get()
			- num_completed_vertices_in_level.get();
	}

	size_t get_num_activated_on_others() {
		return graph->get_num_remaining_vertices();
	}
};

request_range ts_compute_vertex::get_next_request(graph_engine *graph)
{
	ts_vertex_request ts_req(graph);
	get_next_required_ts_vertex(ts_req);
	assert(ts_req.get_edge_type() == edge_type::BOTH_EDGES);

	compute_vertex &info = graph->get_vertex(ts_req.get_id());
	data_loc_t loc(graph->get_file_id(), info.get_ext_mem_off());
	if (ts_req.is_require_all()) {
		return request_range(loc, info.get_ext_mem_size(), READ, NULL);
	}
	else {
		worker_thread *t = (worker_thread *) thread::get_curr_thread();
		compute_allocator *alloc = t->get_part_compute_allocator();
		assert(alloc);
		part_ts_vertex_compute *comp = (part_ts_vertex_compute *) alloc->alloc();
		comp->init(this, ts_req);
		// We assume the header of a ts-vertex is never larger than a page.
		return request_range(loc, PAGE_SIZE, READ, comp);
	}
}

bool ts_compute_vertex::run_on_neighbors(graph_engine &graph,
		const page_vertex *vertices[], int num)
{
	const TS_page_vertex **ts_vertices = (const TS_page_vertex **) vertices;
	return run_on_neighbors(graph, ts_vertices, num);
}

compute_allocator *ts_compute_vertex::create_part_compute_allocator(
		graph_engine *graph, thread *t)
{
	return new vertex_compute_allocator<part_ts_vertex_compute>(
			graph, t);
}

request_range part_ts_vertex_compute::get_next_request()
{
	assert(required_vertex_header);
	assert(num_issued == 0);
	num_issued++;

	compute_vertex &info = graph->get_vertex(
			required_vertex_header->get_id());
	offset_pair rel_offsets = required_vertex_header->get_edge_list_offset(
			required_part.get_range());
	data_loc_t loc(graph->get_file_id(), rel_offsets.first
			+ info.get_ext_mem_off());
	return request_range(loc, rel_offsets.second - rel_offsets.first,
			READ, this);
}

void part_ts_vertex_compute::run(page_byte_array &array)
{
	assert(!has_completed());
	bool completed = false;
	if (required_vertex_header == NULL) {
		ext_mem_vertex_interpreter *interpreter = graph->get_vertex_interpreter();
		char *buf = new char[interpreter->get_vertex_size()];
		required_vertex_header = (const TS_page_vertex *) interpreter->interpret(
				array, buf, interpreter->get_vertex_size());
		assert(!required_vertex_header->is_complete());
	}
	else {
		ext_mem_vertex_interpreter *interpreter = graph->get_vertex_interpreter();
		stack_array<char, 64> buf(interpreter->get_vertex_size());
		const page_vertex *ext_v = interpreter->interpret_part(
				required_vertex_header, array, buf.data(),
				interpreter->get_vertex_size());

		num_fetched++;
		assert(comp_v);
		completed = comp_v->run_on_neighbors(*graph, &ext_v, 1);

		char *tmp = (char *) required_vertex_header;
		delete [] tmp;
	}
	// We need to notify the thread that initiate processing the vertex
	// of the completion of the vertex.
	if (completed)
		issue_thread->complete_vertex(*comp_v);
}

void vertex_compute::run(page_byte_array &array)
{
	ext_mem_vertex_interpreter *interpreter = graph->get_vertex_interpreter();
	stack_array<char, 64> buf(interpreter->get_vertex_size());
	const page_vertex *ext_v = interpreter->interpret(array, buf.data(),
			interpreter->get_vertex_size());
	bool completed = false;
	// If the algorithm doesn't need to get the full information
	// of their neighbors
	if (graph->get_required_neighbor_type() == edge_type::NONE
			// Or we haven't perform computation on the vertex yet.
			|| v == NULL) {
		v = &graph->get_vertex(ext_v->get_id());
		completed = v->run(*graph, ext_v);
	}
	else {
		num_complete_fetched++;
		completed = v->run_on_neighbors(*graph, &ext_v, 1);
	}
	// We need to notify the thread that initiate processing the vertex
	// of the completion of the vertex.
	if (completed) {
		// If the vertex has completed computation, the user compute should
		// also finish its computation.
		assert(has_completed());
		issue_thread->complete_vertex(*v);
	}
}

worker_thread::worker_thread(graph_engine *graph, file_io_factory *factory,
		int node_id, int worker_id, int num_threads): thread("worker_thread",
			node_id), msg_q(get_node_id(), "graph_msg_queue", 16, INT_MAX),
		next_activated_vertices((size_t) ceil(((double) graph->get_max_vertex_id()
						+ 1) / num_threads))
{
	start_all = false;
	this->worker_id = worker_id;
	this->graph = graph;
	this->io = NULL;
	this->factory = factory;
	alloc = new vertex_compute_allocator<vertex_compute>(graph, this);
	// TODO we need to fix this.
	part_alloc = graph->create_part_compute_allocator(this);
}

worker_thread::~worker_thread()
{
	delete alloc;
	delete msg_alloc;
	for (unsigned i = 0; i < msg_senders.size(); i++)
		simple_msg_sender::destroy(msg_senders[i]);
	for (unsigned i = 0; i < multicast_senders.size(); i++)
		multicast_msg_sender::destroy(multicast_senders[i]);
	for (unsigned i = 0; i < activate_senders.size(); i++)
		multicast_msg_sender::destroy(activate_senders[i]);
	graph->destroy_part_compute_allocator(part_alloc);
	factory->destroy_io(io);
}

void worker_thread::init_messaging(const std::vector<worker_thread *> &threads)
{
	steal_thread_id = (worker_id + 1) % threads.size();
	// We increase the allocator by 1M each time.
	// It shouldn't need to allocate much memory.
	msg_alloc = new slab_allocator("graph-message-allocator",
			GRAPH_MSG_BUF_SIZE, 1024 * 1024, INT_MAX, get_node_id());

	int num_self = 0;
	for (unsigned i = 0; i < threads.size(); i++) {
		if (threads[i] == this)
			num_self++;
		msg_senders.push_back(simple_msg_sender::create(get_node_id(),
					msg_alloc, &threads[i]->msg_q));
		multicast_senders.push_back(multicast_msg_sender::create(msg_alloc,
					&threads[i]->msg_q));
		activate_senders.push_back(multicast_msg_sender::create(msg_alloc,
					&threads[i]->msg_q));
	}
	assert(num_self == 1);
}

/**
 * This steals vertices from other threads. It tries to steal more vertices
 * than it can process, and the remaining vertices will be placed in its
 * own activated vertex queue.
 */
int worker_thread::steal_activated_vertices(vertex_id_t vertex_buf[], int buf_size)
{
	if (steal_thread_id == this->worker_id)
		steal_thread_id = (steal_thread_id + 1) % graph->get_num_threads();
	int num_tries = 0;
	vertex_id_t *steal_buf = new vertex_id_t[MAX_STOLEN_VERTICES];
	int num;
	do {
		worker_thread *t = graph->get_thread(steal_thread_id);
		num_tries++;

		// We want to steal as much as possible, but we don't want
		// to overloaded by the stolen vertices.
		size_t num_steal = max(1,
				t->curr_activated_vertices.get_num_vertices() / graph->get_num_threads());
		num = t->curr_activated_vertices.fetch(steal_buf,
				min(MAX_STOLEN_VERTICES, num_steal));

		// If we can't steal vertices from the thread, we should move
		// to the next thread.
		if (num == 0)
			steal_thread_id = (steal_thread_id + 1) % graph->get_num_threads();
		// If we have tried to steal vertices from all threads.
	} while (num == 0 && num_tries < graph->get_num_threads());

	int ret = min(buf_size, num);
	memcpy(vertex_buf, steal_buf, sizeof(vertex_buf[0]) * ret);
	// We stole more vertices than we can process this time.
	// The vertices stolen from another thread will also be placed in
	// the queue for currently activated vertices.
	if (num - ret > 0)
		curr_activated_vertices.init(steal_buf + ret, num - ret, true);
	delete [] steal_buf;
	return ret;
}

/**
 * This is to process the activated vertices in the current iteration.
 */
int worker_thread::process_activated_vertices(int max)
{
	if (max <= 0)
		return 0;

	vertex_id_t vertex_buf[max];
	stack_array<io_request> reqs(max);
	int num = curr_activated_vertices.fetch(vertex_buf, max);
	if (num == 0)
		num = steal_activated_vertices(vertex_buf, max);
	if (num > 0) {
		num_activated_vertices_in_level.inc(num);
		graph->process_vertices(num);
	}

	int num_completed = 0;
	int num_to_process = 0;
	for (int i = 0; i < num; i++) {
		compute_vertex &info = graph->get_vertex(vertex_buf[i]);
		// We need to execute the pre-run to determine if we should
		// fetch the adjacency list of itself.
		if (info.run(*graph)) {
			data_loc_t loc(io->get_file_id(), info.get_ext_mem_off());
			reqs[num_to_process++] = io_request(alloc->alloc(), loc,
					// TODO I might need to set the node id.
					info.get_ext_mem_size(), READ, io, -1);
		}
		else
			num_completed++;
	}
	if (num_completed > 0)
		num_completed_vertices_in_level.inc(num_completed);
	if (graph->get_logger())
		graph->get_logger()->log(reqs.data(), num_to_process);
	io->access(reqs.data(), num_to_process);
	return num;
}

int worker_thread::enter_next_level()
{
	// We have to make sure all messages sent by other threads are processed.
	process_msgs();
	curr_activated_vertices.init(next_activated_vertices, worker_id,
			graph->get_partitioner());
	next_activated_vertices.clear();
	return curr_activated_vertices.get_num_vertices();
}

void worker_thread::process_multicast_msg(multicast_message &mmsg)
{
	int num_dests = mmsg.get_num_dests();
	multicast_dest_list dest_list = mmsg.get_dest_list();
	for (int i = 0; i < num_dests; i++) {
		vertex_id_t id = dest_list.get_dest(i);
		int part_id;
		off_t off;
		graph->get_partitioner()->map2loc(id, part_id, off);
		assert(part_id == worker_id);
		// TODO now the size is the entire message. Now the message
		// is considered as non-empty.
		if (!mmsg.is_empty()) {
			compute_vertex &info = graph->get_vertex(id);
			const vertex_message *msgs[] = {&mmsg};
			info.run_on_messages(*graph, msgs, 1);
		}
		if (mmsg.is_activate())
			next_activated_vertices.set(off);
	}
}

void worker_thread::process_msg(message &msg)
{
	const int VMSG_BUF_SIZE = 128;
	vertex_message *v_msgs[VMSG_BUF_SIZE];
	while (!msg.is_empty()) {
		int num = msg.get_next(v_msgs, VMSG_BUF_SIZE);
		for (int i = 0; i < num; i++) {
			if (v_msgs[i]->is_multicast()) {
				process_multicast_msg(*multicast_message::cast2multicast(
							v_msgs[i]));
				continue;
			}
			vertex_id_t id = v_msgs[i]->get_dest();
			int part_id;
			off_t off;
			graph->get_partitioner()->map2loc(id, part_id, off);
			assert(part_id == worker_id);
			if (!v_msgs[i]->is_empty()) {
				compute_vertex &info = graph->get_vertex(id);
				info.run_on_messages(*graph,
						(const vertex_message **) &v_msgs[i], 1);
			}
			if (v_msgs[i]->is_activate())
				next_activated_vertices.set(off);
		}
	}
}

void worker_thread::process_msgs()
{
	const int MSG_BUF_SIZE = 16;
	message msgs[MSG_BUF_SIZE];
	while (!msg_q.is_empty()) {
		int num_fetched = msg_q.fetch(msgs, MSG_BUF_SIZE);
		for (int i = 0; i < num_fetched; i++)
			process_msg(msgs[i]);
	}
}

/**
 * This method is the main function of the graph engine.
 */
void worker_thread::run()
{
	while (true) {
		int num_visited = 0;
		int num;
		do {
			num = process_activated_vertices(
					graph_conf.get_max_processing_vertices()
					- io->num_pending_ios());
			num_visited += num;
			process_msgs();
			io->wait4complete(min(io->num_pending_ios() / 10, 2));
			// If there are vertices being processed, we need to call
			// wait4complete to complete processing them.
		} while (get_num_vertices_processing() > 0
				// We still have vertices remaining for processing
				|| !curr_activated_vertices.is_empty()
				// Even if we have processed all activated vertices belonging
				// to this thread, we still need to process vertices from
				// other threads in order to balance the load.
				|| get_num_activated_on_others() > 0);
		assert(curr_activated_vertices.is_empty());
		printf("thread %d visited %d vertices\n", this->get_id(), num_visited);

		// Now we have finished this level, we can progress to the next level.
		num_activated_vertices_in_level = atomic_number<long>(0);
		num_completed_vertices_in_level = atomic_number<long>(0);

		flush_msgs();
		bool completed = graph->progress_next_level();
		printf("thread %d finish in a level, completed? %d\n", get_id(), completed);
		if (completed)
			break;
	}
	stop();
	if (graph_conf.get_print_io_stat())
		io->print_stat(graph->get_num_threads());
}

graph_engine::graph_engine(int num_threads, int num_nodes,
		const std::string &graph_file, graph_index *index)
{
	this->scheduler = &default_scheduler;
	this->partitioner = new vertex_partitioner(num_threads);
	this->required_neighbor_type = edge_type::NONE;
	is_complete = false;
	this->vertices = index;

	pthread_mutex_init(&lock, NULL);
	pthread_barrier_init(&barrier1, NULL, num_threads);
	pthread_barrier_init(&barrier2, NULL, num_threads);

	// Right now only the cached I/O can support async I/O
	file_io_factory *factory = create_io_factory(graph_file,
			GLOBAL_CACHE_ACCESS);
	factory->set_sched_creater(new throughput_comp_io_sched_creater());

	io_interface *io = factory->create_io(thread::get_curr_thread());
	io->access((char *) &header, 0, sizeof(header), READ);
	header.verify();
	factory->destroy_io(io);

	switch (header.get_graph_type()) {
		case graph_type::DIRECTED:
			interpreter = new ext_mem_directed_vertex_interpreter();
			break;
		case graph_type::UNDIRECTED:
			interpreter = new ext_mem_undirected_vertex_interpreter();
			break;
		case graph_type::TS_DIRECTED:
			interpreter = new ts_ext_mem_vertex_interpreter(
					header.get_max_num_timestamps());
			break;
		case graph_type::TS_UNDIRECTED:
			assert(0);
			break;
		default:
			assert(0);
	}

	file_id = factory->get_file_id();
	assert(num_threads > 0 && num_nodes > 0);
	assert(num_threads % num_nodes == 0);
	for (int i = 0; i < num_threads; i++) {
		worker_thread *t = new worker_thread(this, factory,
				i % num_nodes, i, num_threads);
		worker_threads.push_back(t);
	}
	for (int i = 0; i < num_threads; i++) {
		worker_threads[i]->init_messaging(worker_threads);
	}
	first_thread = worker_threads[0];

	if (graph_conf.get_trace_file().empty())
		logger = NULL;
	else
		logger = new trace_logger(graph_conf.get_trace_file());
}

graph_engine::~graph_engine()
{
	delete partitioner;
	for (unsigned i = 0; i < worker_threads.size(); i++)
		delete worker_threads[i];
	if (logger)
		delete logger;
}

multicast_msg_sender *graph_engine::get_activate_sender(int thread_id) const
{
	worker_thread *curr = (worker_thread *) thread::get_curr_thread();
	return curr->get_activate_sender(thread_id);
}

multicast_msg_sender *graph_engine::get_multicast_sender(int thread_id) const
{
	worker_thread *curr = (worker_thread *) thread::get_curr_thread();
	return curr->get_multicast_sender(thread_id);
}

simple_msg_sender *graph_engine::get_msg_sender(int thread_id) const
{
	worker_thread *curr = (worker_thread *) thread::get_curr_thread();
	return curr->get_msg_sender(thread_id);
}

void graph_engine::start(vertex_id_t ids[], int num)
{
	num_remaining_vertices_in_level.inc(num);
	int num_threads = get_num_threads();
	std::vector<vertex_id_t> start_vertices[num_threads];
	for (int i = 0; i < num; i++) {
		int idx = get_partitioner()->map(ids[i]);
		start_vertices[idx].push_back(ids[i]);
	}
	for (int i = 0; i < num_threads; i++) {
		worker_threads[i]->start_vertices(start_vertices[i]);
		worker_threads[i]->start();
	}
}

void graph_engine::start_all()
{
	num_remaining_vertices_in_level.inc(get_num_vertices());
	BOOST_FOREACH(worker_thread *t, worker_threads) {
		t->start_all_vertices();
		t->start();
	}
}

bool graph_engine::progress_next_level()
{
	static atomic_number<long> tot_num_activates;
	static atomic_integer num_threads;
	// We have to make sure all threads have reach here, so we can switch
	// queues to progress to the next level.
	// If the queue of the next level is empty, the program can terminate.
	int rc = pthread_barrier_wait(&barrier1);
	if(rc != 0 && rc != PTHREAD_BARRIER_SERIAL_THREAD)
	{
		printf("Could not wait on barrier\n");
		exit(-1);
	}
	worker_thread *curr = (worker_thread *) thread::get_curr_thread();
	int num_activates = curr->enter_next_level();
	tot_num_activates.inc(num_activates);
	// If all threads have reached here.
	if (num_threads.inc(1) == get_num_threads()) {
		level.inc(1);
		printf("progress to level %d, there are %ld vertices in this level\n",
				level.get(), tot_num_activates.get());
		assert(num_remaining_vertices_in_level.get() == 0);
		num_remaining_vertices_in_level = atomic_number<size_t>(
				tot_num_activates.get());
		// If there aren't more activated vertices.
		is_complete = tot_num_activates.get() == 0;
		tot_num_activates = 0;
		num_threads = 0;
	}

	// We need to synchronize again. We have to make sure all threads see
	// the completion signal.
	rc = pthread_barrier_wait(&barrier2);
	if(rc != 0 && rc != PTHREAD_BARRIER_SERIAL_THREAD)
	{
		printf("Could not wait on barrier\n");
		exit(-1);
	}
	return is_complete;
}

void graph_engine::wait4complete()
{
	for (unsigned i = 0; i < worker_threads.size(); i++) {
		worker_threads[i]->join();
	}
}

void graph_engine::set_vertex_scheduler(vertex_scheduler *scheduler)
{
	for (size_t i = 0; i < worker_threads.size(); i++)
		worker_threads[i]->set_vertex_scheduler(scheduler);
}

vertex_index *load_vertex_index(const std::string &index_file)
{
	const int INDEX_HEADER_SIZE = PAGE_SIZE * 2;
	const int READ_SIZE = 100 * 1024 * 1024;

	// Right now only the cached I/O can support async I/O
	file_io_factory *factory = create_io_factory(index_file,
			REMOTE_ACCESS);
	assert(factory->get_file_size() >= INDEX_HEADER_SIZE);
	io_interface *io = factory->create_io(thread::get_curr_thread());

	// Get the header of the index.
	char *tmp = NULL;
	int ret = posix_memalign((void **) &tmp, PAGE_SIZE, INDEX_HEADER_SIZE);
	assert(ret == 0);
	data_loc_t loc(factory->get_file_id(), 0);
	io_request req(tmp, loc, INDEX_HEADER_SIZE, READ, io, -1);
	io->access(&req, 1);
	io->wait4complete(1);
	vertex_index *index = (vertex_index *) tmp;
	index->get_graph_header().verify();

	// Initialize the buffer for containing the index.
	size_t index_size = index->get_index_size();
	assert((ssize_t) index_size <= factory->get_file_size());
	char *buf = NULL;
	ret = posix_memalign((void **) &buf, PAGE_SIZE, index_size);
	assert(ret == 0);
	off_t off = 0;
	memcpy(buf, tmp, INDEX_HEADER_SIZE);
	off += INDEX_HEADER_SIZE;
	free(tmp);

	// Read the index to the memory.
	size_t aligned_index_size = ROUND_PAGE(index_size);
	while ((size_t) off < aligned_index_size) {
		assert(off % PAGE_SIZE == 0);
		size_t size = min(READ_SIZE, aligned_index_size - off);
		data_loc_t loc(factory->get_file_id(), off);
		io_request req(buf + off, loc, size, READ, io, -1);
		io->access(&req, 1);
		off += size;
		if (io->num_pending_ios() > 100)
			io->wait4complete(io->num_pending_ios() / 10);
	}
	io->wait4complete(io->num_pending_ios());

	// Read the last page.
	// The data may only occupy part of the page.
	if (aligned_index_size < index_size) {
		char *tmp = NULL;
		int ret = posix_memalign((void **) &tmp, PAGE_SIZE, PAGE_SIZE);
		assert(ret == 0);
		data_loc_t loc(factory->get_file_id(), aligned_index_size);
		io_request req(tmp, loc, PAGE_SIZE, READ, io, -1);
		io->access(&req, 1);
		io->wait4complete(1);
		memcpy(buf + aligned_index_size, tmp, index_size - aligned_index_size);
		free(tmp);
	}
	factory->destroy_io(io);

	index = (vertex_index *) buf;
	if (index->get_graph_header().get_graph_type() == graph_type::DIRECTED)
		((directed_vertex_index *) index)->verify();
	else
		((default_vertex_index *) index)->verify();
	return index;
}
