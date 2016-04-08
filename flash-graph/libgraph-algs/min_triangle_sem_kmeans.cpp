/*
 * Copyright 2015 Open Connectome Project (http://openconnecto.me)
 * Written by Disa Mhembere (disa@jhu.edu)
 *
 * This file is part of FlashGraph.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY CURRENT_KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <signal.h>
#ifdef PROFILER
#include <gperftools/profiler.h>
#endif

#include "sem_kmeans.h"
#include "clusters.h"
#include "row_cache.h"
#include "matrix/kmeans.h"

using namespace fg;

namespace {

#if KM_TEST
    static prune_stats::ptr g_prune_stats;
    static std::vector<double> g_gb_req_iter; // GB req per iter
    static std::vector<size_t> g_gb_obt_iter; // GB data obtained / iter
    static std::vector<size_t> g_cache_hits_iter; // cache hits / iter
    static activation_counter::ptr acntr; // How many are active per iteration
    static active_counter::ptr ac;
#endif
    static size_t g_io_reqs = 0;

    static bool g_prune_init = false;
    static prune::dist_matrix::ptr g_cluster_dist;
    static prune_clusters::ptr g_clusters; // cluster means/centers

    static unsigned NUM_ROWS;
    static unsigned g_num_changed = 0;
    static struct timeval start, end;
    static init_type_t g_init; // May have to use
    static unsigned  g_kmspp_cluster_idx; // Used for kmeans++ init
    static unsigned g_kmspp_next_cluster; // Sample row selected as next cluster
    static kmspp_stage_t g_kmspp_stage; // Either adding a mean / computing dist
    static kms_stage_t g_stage; // What phase of the algo we're in
    static unsigned g_iter;

    static partition_cache<double>::ptr g_row_cache = nullptr;
    static unsigned g_io_iter = 0; // How many iterations of full I/O have I done?
    static unsigned g_row_cache_size = 0;
    static unsigned g_nthread;
    static std::vector<std::vector<double>> g_data;
    static unsigned g_cache_update_iter = 5; // TODO: select param

    class kmeans_vertex: public base_kmeans_vertex
    {
        bool recalculated;
        double dist;

        public:
        kmeans_vertex(vertex_id_t id): base_kmeans_vertex(id) {
            recalculated = false;
            dist = std::numeric_limits<double>::max(); // Start @ max
        }

        void run(vertex_program &prog);

        void run(vertex_program& prog, const page_vertex &vertex) {
            switch (g_stage) {
                case INIT:
                    run_init(prog, vertex, g_init);
                    break;
                case ESTEP:
                    run_distance(prog, vertex);
                    break;
                default:
                    BOOST_ASSERT_MSG(0, "Unknown g_stage!");
            }
        }

        const double get_dist() const { return this->dist; }
        void set_dist(const double dist) { this->dist = dist; }

        void run_on_message(vertex_program& prog, const vertex_message& msg) { }
        void run_init(vertex_program& prog, const page_vertex &vertex, init_type_t init);
        void run_init(vertex_program& prog, const double* row, init_type_t init);


        void run_distance(vertex_program& prog, const page_vertex& vertex);
        void run_distance(vertex_program& prog, const double* row);
    };

    class kmeans_vertex_program:
        public base_kmeans_vertex_program<kmeans_vertex, clusters>
    {
        unsigned num_reqs;
#if KM_TEST
        prune_stats::ptr pt_ps;
#endif

        public:
        typedef std::shared_ptr<kmeans_vertex_program> ptr;

        kmeans_vertex_program() {
            this->num_reqs = 0;
#if KM_TEST
            pt_ps = prune_stats::create(NUM_ROWS, K);
#endif
        }

        static ptr cast2(vertex_program::ptr prog) {
            return std::static_pointer_cast<kmeans_vertex_program, vertex_program>(prog);
        }

        void remove_member(const unsigned id, data_seq_iter& count_it) {
            get_pt_clusters()->remove_member(count_it, id);
        }

        void swap_membership(data_seq_iter& count_it, const unsigned from_id,
                const unsigned to_id) {
            get_pt_clusters()->swap_membership(count_it, from_id, to_id);
        }

        void swap_membership(const double* row, const unsigned from_id,
                const unsigned to_id) {
            get_pt_clusters()->swap_membership<double>(row, from_id, to_id);
        }

#if KM_TEST
        prune_stats::ptr get_ps() { return pt_ps; }
#endif
        void num_requests_pp() {
            num_reqs++;
        }
        const unsigned get_num_reqs() const { return num_reqs; }
    };

    class kmeans_vertex_program_creater: public vertex_program_creater
    {
        public:
            vertex_program::ptr create() const {
                return vertex_program::ptr(new kmeans_vertex_program());
            }
    };

    /* Used in kmeans++ initialization */
    class kmeanspp_vertex_program : public vertex_program_impl<kmeans_vertex>
    {
        double pt_cuml_sum;

        public:
        typedef std::shared_ptr<kmeanspp_vertex_program> ptr;

        kmeanspp_vertex_program() {
            pt_cuml_sum = 0.0;
        }

        static ptr cast2(vertex_program::ptr prog) {
            return std::static_pointer_cast<kmeanspp_vertex_program,
                   vertex_program>(prog);
        }

        void pt_cuml_sum_peq (const double val) {
            pt_cuml_sum += val;
        }

        const double get_pt_cuml_sum() const {
            return pt_cuml_sum;
        }
    };

    class kmeanspp_vertex_program_creater: public vertex_program_creater
    {
        public:
            vertex_program::ptr create() const {
                return vertex_program::ptr(new kmeanspp_vertex_program());
            }
    };

    void kmeans_vertex::run(vertex_program &prog) {
        if (g_kmspp_stage == DIST) {
            if (get_cluster_id() != INVALID_CLUST_ID &&
                    get_dist() <= g_cluster_dist->get(get_cluster_id(),
                        g_kmspp_cluster_idx)) {
                // No dist comp, but add my mean
                ((kmeanspp_vertex_program&)prog).
                    pt_cuml_sum_peq(get_dist());
                return;
            }
        } else if (g_stage != INIT) {
            recalculated = false;
            if (!g_prune_init) {
                set_dist(get_dist() + g_clusters->get_prev_dist(get_cluster_id()));

                if (get_dist() <= g_clusters->get_s_val(get_cluster_id())) {
#if KM_TEST
                    ((kmeans_vertex_program&) prog).get_ps()->pp_lemma1(K);
#endif
#if VERBOSE
                    ac->is_active(prog.get_vertex_id(*this), false);
#endif
                    return; // Nothing changes -- no I/O request!
                }
            }
#if VERBOSE
            ac->is_active(prog.get_vertex_id(*this), true);
#endif
        }

        vertex_id_t id = prog.get_vertex_id(*this);
        if (g_row_cache) {
            const unsigned thd = prog.get_partition_id();
            double* row = g_row_cache->get(id, thd);
#if KM_TEST
            acntr->active(thd);
#endif
            if (row) { // row == NULL is a cache miss
                switch (g_stage) {
                    case INIT:
                        run_init(prog, row, g_init);
                        break;
                    case ESTEP:
                        run_distance(prog, row);
                        break;
                    default:
                        BOOST_ASSERT_MSG(0, "Unknown g_stage!");
                }
                return;
            }
        }

        if (g_stage != INIT)
            ((kmeans_vertex_program&) prog).num_requests_pp();

        request_vertices(&id, 1);
    }

    double dist_comp(const page_vertex &vertex, const double* mean,
            vertex_id_t my_id=-1, unsigned thd=-1) {
        data_seq_iter count_it =
            ((const page_row&)vertex).get_data_seq_it<double>();

        double dist = 0;
        double diff;
        vertex_id_t nid = 0;

        if (g_row_cache && g_row_cache->add_id(thd, my_id)) {
            while(count_it.has_next()) {
                double e = count_it.next();
                g_row_cache->add(thd, e, ((nid+1) == NUM_COLS));
                diff = e - mean[nid++];
                dist += diff*diff;
            }
        } else {
            while(count_it.has_next()) {
                double e = count_it.next();
                diff = e - mean[nid++];
                dist += diff*diff;
            }
        }
        BOOST_VERIFY(nid == NUM_COLS);
        return sqrt(dist); // TODO: sqrt
    }

    void kmeans_vertex::run_init(vertex_program& prog, const double* row,
            init_type_t init) {
        switch (g_init) {
            case RANDOM:
                {
                    unsigned new_cluster_id = random() % K;
                    kmeans_vertex_program& vprog = (kmeans_vertex_program&) prog;
#if VERBOSE
                    printf("Random init: v%u assigned to cluster: c%x\n",
                            prog.get_vertex_id(*this), new_cluster_id);
#endif
                    set_cluster_id(new_cluster_id);
                    vprog.add_member(get_cluster_id(), row);
                }
                break;
            case FORGY:
                {
                    vertex_id_t my_id = prog.get_vertex_id(*this);
#if KM_TEST
                    printf("Forgy init: v%u setting cluster: c%x\n", my_id, g_init_hash[my_id]);
#endif
                    g_clusters->set_mean(row, g_init_hash[my_id]);
                }
                break;
            case PLUSPLUS:
                {
                    if (g_kmspp_stage == ADDMEAN) {
#if KM_TEST
                        vertex_id_t my_id = prog.get_vertex_id(*this);
                        printf("kms++ v%u making itself c%u\n", my_id, g_kmspp_cluster_idx);
#endif
                        g_clusters->add_member(row, g_kmspp_cluster_idx);
                    } else if (g_kmspp_stage == DIST) {
                        vertex_id_t my_id = prog.get_vertex_id(*this);

                        if (get_cluster_id() != INVALID_CLUST_ID &&
                                g_kmspp_distance[my_id] <=
                                g_cluster_dist->get(g_kmspp_cluster_idx, get_cluster_id())) {
                        } else {
                            double _dist = dist_comp_raw(row, &(g_clusters->get_means()
                                        [g_kmspp_cluster_idx*NUM_COLS]), NUM_COLS);

                            if (_dist < g_kmspp_distance[my_id]) {
                                g_kmspp_distance[my_id] = _dist;
                                set_cluster_id(g_kmspp_cluster_idx);
                                set_dist(_dist);
                            }
                        }
                        ((kmeanspp_vertex_program&)prog).
                            pt_cuml_sum_peq(g_kmspp_distance[my_id]);
                    } else {
                        BOOST_ASSERT_MSG(0, "Unknown g_kmspp_stage type");
                    }
                }
                break;
            default:
                BOOST_ASSERT_MSG(0, "Unknown g_init type");
        }
    }

    void add_row(const page_vertex& vertex, const vertex_id_t id) {
        data_seq_iter it = ((const page_row&)vertex).
            get_data_seq_it<double>();
        std::vector<double> v;
        while (it.has_next()) {
            double el = it.next();
            v.push_back(el);
        }
        g_data[id] = v;
    }

    void kmeans_vertex::run_init(vertex_program& prog,
            const page_vertex &vertex, init_type_t init) {
        switch (g_init) {
            case RANDOM:
                {
                    unsigned new_cluster_id = random() % K;
                    kmeans_vertex_program& vprog = (kmeans_vertex_program&) prog;
#if VERBOSE
                    printf("Random init: v%u assigned to cluster: c%x\n",
                            prog.get_vertex_id(*this), new_cluster_id);
#endif
                    set_cluster_id(new_cluster_id);
                    data_seq_iter count_it = ((const page_row&)vertex).
                        get_data_seq_it<double>();
                    vprog.add_member(get_cluster_id(), count_it);
                }
                break;
            case FORGY:
                {
                    vertex_id_t my_id = prog.get_vertex_id(*this);
#if KM_TEST
                    printf("Forgy init: v%u setting cluster: c%x\n", my_id, g_init_hash[my_id]);
#endif
                    data_seq_iter count_it = ((const page_row&)vertex).
                        get_data_seq_it<double>();
                    g_clusters->set_mean(count_it, g_init_hash[my_id]);
                }
                break;
            case PLUSPLUS:
                {
                    data_seq_iter count_it = ((const page_row&)vertex).
                        get_data_seq_it<double>();

                    if (g_kmspp_stage == ADDMEAN) {
#if KM_TEST
                        vertex_id_t my_id = prog.get_vertex_id(*this);
                        printf("kms++ v%u making itself c%u\n", my_id, g_kmspp_cluster_idx);
#endif
                        g_clusters->add_member(count_it, g_kmspp_cluster_idx);
                    } else if (g_kmspp_stage == DIST) {
                        vertex_id_t my_id = prog.get_vertex_id(*this);
                        unsigned thd = -1;

                        //if (g_kmspp_cluster_idx == 0)
                        //add_row(vertex, my_id);

                        if (g_row_cache)
                            thd = prog.get_partition_id();

                        if (get_cluster_id() != INVALID_CLUST_ID &&
                                g_kmspp_distance[my_id] <=
                                g_cluster_dist->get(g_kmspp_cluster_idx, get_cluster_id())) {
                        } else {
                            double _dist = dist_comp(vertex,
                                    &(g_clusters->get_means()[g_kmspp_cluster_idx*NUM_COLS]),
                                    my_id, thd);

                            if (_dist < g_kmspp_distance[my_id]) {
                                g_kmspp_distance[my_id] = _dist;
                                set_cluster_id(g_kmspp_cluster_idx);
                                set_dist(_dist);
                            }
                        }
                        ((kmeanspp_vertex_program&)prog).
                            pt_cuml_sum_peq(g_kmspp_distance[my_id]);
                    } else {
                        BOOST_ASSERT_MSG(0, "Unknown g_kmspp_stage type");
                    }
                }
                break;
            default:
                BOOST_ASSERT_MSG(0, "Unknown g_init type");
        }
    }

    void kmeans_vertex::run_distance(vertex_program& prog, const double* row) {
        kmeans_vertex_program& vprog = (kmeans_vertex_program&) prog;
        unsigned old_cluster_id = get_cluster_id();

        if (g_prune_init) {
            for (unsigned cl = 0; cl < K; cl++) {
                double udist = dist_comp_raw(row, &(g_clusters->get_means()[cl*NUM_COLS]),
                        NUM_COLS);
                if (udist < get_dist()) {
                    set_dist(udist);
                    set_cluster_id(cl);
                }
            }
        } else {
            for (unsigned cl = 0; cl < K; cl++) {
                if (get_dist() <= g_cluster_dist->get(get_cluster_id(), cl)) {
#if KM_TEST
                    vprog.get_ps()->pp_3a();
#endif
                    continue;
                }

                // If not recalculated to my current cluster .. do so to tighten bounds
                if (!recalculated) {
                    double udist = dist_comp_raw(row, &(g_clusters->get_means()
                                [get_cluster_id()*NUM_COLS]), NUM_COLS);
                    set_dist(udist);
                    recalculated = true;
                }

                if (get_dist() <= g_cluster_dist->get(get_cluster_id(), cl)) {
#if KM_TEST
                    vprog.get_ps()->pp_3c();
#endif
                    continue;
                }

                // Track 5
                double jdist = dist_comp_raw(row, &(g_clusters->get_means()[cl*NUM_COLS]),
                        NUM_COLS);
                if (jdist < get_dist()) {
                    set_dist(jdist);
                    set_cluster_id(cl);
                }
            }
        }

#if KM_TEST
        BOOST_VERIFY(get_cluster_id() >= 0 && get_cluster_id() < K);
#endif
        if (g_prune_init) {
            vprog.pt_changed_pp(); // Add a vertex to the count of changed ones
            vprog.add_member(get_cluster_id(), row);
        } else if (old_cluster_id != get_cluster_id()) {
            vprog.pt_changed_pp(); // Add a vertex to the count of changed ones
            vprog.swap_membership(row, old_cluster_id, get_cluster_id());
        }
    }

    void kmeans_vertex::run_distance(vertex_program& prog, const page_vertex& vertex) {
        kmeans_vertex_program& vprog = (kmeans_vertex_program&) prog;
        unsigned old_cluster_id = get_cluster_id();

        vertex_id_t my_id = -1;
        unsigned thd = -1;

        if (g_row_cache) {
            my_id = prog.get_vertex_id(*this);
            thd = prog.get_partition_id();
        }

        if (g_prune_init) {
            for (unsigned cl = 0; cl < K; cl++) {
                double udist = dist_comp(vertex,
                        &(g_clusters->get_means()[cl*NUM_COLS]), my_id, thd);
                if (udist < get_dist()) {
                    set_dist(udist);
                    set_cluster_id(cl);
                }
            }
        } else {
            for (unsigned cl = 0; cl < K; cl++) {
                if (get_dist() <= g_cluster_dist->get(get_cluster_id(), cl)) {
#if KM_TEST
                    vprog.get_ps()->pp_3a();
#endif
                    continue;
                }

                // If not recalculated to my current cluster .. do so to tighten bounds
                if (!recalculated) {
                    double udist = dist_comp(vertex,
                            &(g_clusters->get_means()[get_cluster_id()*NUM_COLS])
                            , my_id, thd);
                    set_dist(udist);
                    recalculated = true;
                }

                if (get_dist() <= g_cluster_dist->get(get_cluster_id(), cl)) {
#if KM_TEST
                    vprog.get_ps()->pp_3c();
#endif
                    continue;
                }

                // Track 5
                double jdist = dist_comp(vertex, &(g_clusters->get_means()[cl*NUM_COLS]),
                        my_id, thd);
                if (jdist < get_dist()) {
                    set_dist(jdist);
                    set_cluster_id(cl);
                }
            }
        }

#if KM_TEST
        BOOST_VERIFY(get_cluster_id() >= 0 && get_cluster_id() < K);
#endif
        data_seq_iter count_it = ((const page_row&)vertex).get_data_seq_it<double>();

        if (g_prune_init) {
            vprog.pt_changed_pp(); // Add a vertex to the count of changed ones
            vprog.add_member(get_cluster_id(), count_it);
        } else if (old_cluster_id != get_cluster_id()) {
            vprog.pt_changed_pp(); // Add a vertex to the count of changed ones
            vprog.swap_membership(count_it, old_cluster_id, get_cluster_id());
        }
    }

    template<class T, class VertexType>
        class dist_query: public fg::vertex_query {
            typename fg::FG_vector<T>::ptr vec;
            public:
            dist_query(typename fg::FG_vector<T>::ptr vec) {
                this->vec = vec;
            }

            virtual void run(fg::graph_engine &graph, fg::compute_vertex &v1) {
                VertexType &v = (VertexType &) v1;
                vec->set(graph.get_graph_index().get_vertex_id(v), v.get_dist());
            }

            virtual void merge(fg::graph_engine &graph, fg::vertex_query::ptr q) {
            }

            virtual ptr clone() {
                return fg::vertex_query::ptr(new dist_query(vec));
            }
        };

    FG_vector<double>::ptr get_dist_v(graph_engine::ptr mat) {
        FG_vector<double>::ptr vec = FG_vector<double>::create(mat);
        mat->query_on_all(vertex_query::ptr(new dist_query<double, kmeans_vertex>(vec)));
        return vec;
    }

    double get_bic(graph_engine::ptr mat) {
        FG_vector<double>::ptr vec = FG_vector<double>::create(mat);
        mat->query_on_all(vertex_query::ptr(new dist_query<double, kmeans_vertex>(vec)));
        return 2*vec->sum() + log(NUM_ROWS)*K*NUM_COLS;
    }

    static FG_vector<unsigned>::ptr get_membership(graph_engine::ptr mat) {
        FG_vector<unsigned>::ptr vec = FG_vector<unsigned>::create(mat);
        mat->query_on_all(vertex_query::ptr(new save_query<unsigned, kmeans_vertex>(vec)));
        return vec;
    }

    static void clear_clusters() {
        if (g_prune_init) {
            g_clusters->clear();
        } else {
            g_clusters->set_prev_means();
            for (unsigned cl = 0; cl < K; cl++) {
                g_clusters->unfinalize(cl);
#if VERBOSE
                std::cout << "Unfinalized g_clusters[thd] ==> ";
                print_vector<double>(g_clusters[cl]->get_mean());
#endif
            }
        }
    }

    // logarithmically increasing update interval
    static void manage_cache() {
        // clear the cache
        printf("\ng_io_iter = %u\n", g_io_iter);
        if (g_row_cache) {
            if (g_io_iter > 0 && (g_io_iter % g_cache_update_iter == 0)) {
                BOOST_LOG_TRIVIAL(info) << "Clearing the cache ...";
                g_row_cache = partition_cache<double>::create(g_nthread,
                        NUM_COLS, g_row_cache_size/(g_nthread*2), g_row_cache_size);
                // log cache
                if (g_io_iter == g_cache_update_iter)
                    g_cache_update_iter += (g_io_iter + g_cache_update_iter);
                else
                    g_cache_update_iter += g_io_iter;

            } else if (g_row_cache->index_empty()){
                BOOST_LOG_TRIVIAL(info) << "Building cache index ...";
                g_row_cache->build_index();
            }
            g_io_iter++;
        }
    }

    static void update_clusters(graph_engine::ptr mat,
            std::vector<unsigned>& num_members_v) {
        clear_clusters();
        std::vector<vertex_program::ptr> kms_clust_progs;
        mat->get_vertex_programs(kms_clust_progs);

#if KM_TEST
        size_t io_req = 0;
#endif
        for (unsigned thd = 0; thd < kms_clust_progs.size(); thd++) {
            kmeans_vertex_program::ptr kms_prog =
                kmeans_vertex_program::cast2(kms_clust_progs[thd]);
            clusters::ptr pt_clusters = kms_prog->get_pt_clusters();
            g_num_changed += kms_prog->get_pt_changed();

            g_io_reqs += kms_prog->get_num_reqs();

#if KM_TEST
            (*g_prune_stats) += (*kms_prog->get_ps());
            io_req += kms_prog->get_num_reqs();
#endif
            BOOST_VERIFY(g_num_changed <= NUM_ROWS);
            /* Merge the per-thread clusters */
            // TODO: Pool
            g_clusters->peq(pt_clusters);
        }

        for (unsigned cl = 0; cl < K; cl++) {
            g_clusters->finalize(cl);
            num_members_v[cl] = g_clusters->get_num_members(cl);

            g_clusters->set_prev_dist(eucl_dist(&(g_clusters->get_means()[cl*NUM_COLS]),
                        &(g_clusters->get_prev_means()[cl*NUM_COLS]), NUM_COLS), cl);
#if VERBOSE
            BOOST_LOG_TRIVIAL(info) << "Distance to prev mean for c:"
                << cl << " is " << g_clusters->get_prev_dist(cl);
            BOOST_VERIFY(g_clusters->get_num_members(cl) <= (int)NUM_ROWS);
#endif
        }
#if KM_TEST
        int t_members = 0;
        for (unsigned cl = 0; cl < K; cl++) {
            t_members += g_clusters->get_num_members(cl);
            if (t_members > (int) NUM_ROWS) {
                BOOST_LOG_TRIVIAL(error) << "[FATAL]: Too many members cluster: "
                    << cl << "/" << K << " at members = " << t_members;
                BOOST_VERIFY(false);
            }
        }

        if (io_req == 0) io_req = NUM_ROWS; // First iteration
        g_gb_req_iter.push_back((io_req*sizeof(double)*NUM_COLS)/
                (double)(1024*1024*1024));
#endif
    }

    /* During kmeans++ we select a new cluster each iteration
       This step get the next sample selected as a cluster center
       */
    static unsigned kmeanspp_get_next_cluster_id(graph_engine::ptr mat) {
#if KM_TEST
        BOOST_LOG_TRIVIAL(info) << "Assigning new cluster ...";
#endif
        std::vector<vertex_program::ptr> kmspp_progs;
        mat->get_vertex_programs(kmspp_progs);

        double cuml_sum = 0;
        BOOST_FOREACH(vertex_program::ptr vprog, kmspp_progs) {
            kmeanspp_vertex_program::ptr kmspp_prog =
                kmeanspp_vertex_program::cast2(vprog);
            cuml_sum += kmspp_prog->get_pt_cuml_sum();
        }

        cuml_sum = (cuml_sum * ((double)random())) / (RAND_MAX-1.0);
        BOOST_ASSERT_MSG(cuml_sum != 0, "Cumulative sum == 0!");

        g_kmspp_cluster_idx++;

        for (unsigned row = 0; row < NUM_ROWS; row++) {
#if VERBOSE
            BOOST_LOG_TRIVIAL(info) << "cuml_sum = " << cuml_sum;
#endif
            cuml_sum -= g_kmspp_distance[row];
            if (cuml_sum <= 0) {
#if KM_TEST
                BOOST_LOG_TRIVIAL(info) << "Choosing v:" << row
                    << " as center K = " << g_kmspp_cluster_idx;
#endif
                return row;
            }
        }
        BOOST_ASSERT_MSG(false, "Cumulative sum of distances was > than distances!");
        exit(EXIT_FAILURE);
    }

    // Return all the cluster means only
    static void copy_means(std::vector<std::vector<double>>& means) {
        for (unsigned cl = 0; cl < K; cl++) {
            means[cl].resize(NUM_COLS);
            std::copy(&(g_clusters->get_means()[cl*NUM_COLS]),
                    &(g_clusters->get_means()[(cl*NUM_COLS)+NUM_COLS]),
                    means[cl].begin());
        }
    }

    static inline bool fexists(const std::string& name) {
        struct stat buffer;
        return (stat (name.c_str(), &buffer) == 0);
    }

    // The global io returned is an accumulation, give per iter
    std::vector<double> per_iter_from_agg_io(std::vector<size_t>& v) {
        std::vector<double> ret;
        size_t prev_iter_io = v[0];

        // NOTE: Assume first location is correct
        for (unsigned i = 1; i < v.size(); i++) {
            size_t tmp = v[i];
            v[i] -= prev_iter_io;
            ret.push_back(v[i]/((double)1024*1024*1024));
            prev_iter_io = tmp;
        }
        return ret;
    }

    // The global io returned is an accumulation, give per iter
    std::vector<size_t> per_iter_from_agg_cache(std::vector<size_t>& v) {
        std::vector<size_t> ret;
        size_t prev_iter_io = v[0];

        // NOTE: Assume first location is correct
        for (unsigned i = 1; i < v.size(); i++) {
            size_t tmp = v[i];
            v[i] -= prev_iter_io;
            ret.push_back(v[i]);
            prev_iter_io = tmp;
        }
        return ret;
    }
}

namespace fg
{
    sem_kmeans_ret::ptr compute_min_triangle_sem_kmeans(FG_graph::ptr fg, const unsigned k,
            const std::string init, const unsigned max_iters, const double tolerance,
            const unsigned num_rows, const unsigned num_cols, std::vector<double>* centers,
            const double cache_size_gb, const unsigned rc_update_start_interval) {
#ifdef PROFILER
        ProfilerStart("libgraph-algs/min_tri_sem_kmeans.perf");
#endif
        K = k;

        // Check Initialization
        if ((NULL == centers) && init.compare("random") && init.compare("kmeanspp") &&
                init.compare("forgy")) {
            BOOST_LOG_TRIVIAL(fatal)
                << "[ERROR]: init must be one of: 'random', 'forgy', 'kmeanspp'.It is '"
                << init << "'";
            exit(EXIT_FAILURE);
        }

        graph_index::ptr index = NUMA_graph_index<kmeans_vertex>::create(
                fg->get_graph_header());
        graph_engine::ptr mat = fg->create_engine(index);

        NUM_ROWS = mat->get_max_vertex_id() + 1;
        NUM_COLS = num_cols;

        g_nthread = atoi(fg->get_configs()
                ->get_option("threads").c_str());

        // Check k
        if (K > NUM_ROWS || K < 2 || K == (unsigned)-1) {
            BOOST_LOG_TRIVIAL(fatal)
                << "'k' must be between 2 and the number of rows in the matrix " <<
                "k = " << K;
            exit(EXIT_FAILURE);
        }

        BOOST_VERIFY(num_cols > 0);

        BOOST_LOG_TRIVIAL(info) << "Matrix has rows = " << NUM_ROWS << ", cols = " <<
            NUM_COLS;
#if KM_TEST
        g_prune_stats = prune_stats::create(NUM_ROWS, K);
        acntr = activation_counter::create(g_nthread);
#endif
#if VERBOSE
        ac = active_counter::create(NUM_ROWS);
#endif

        /*** Begin VarInit of data structures ***/
        g_dist_type = EUCL; // TODO: Add to params

        if (cache_size_gb > 0) {
            g_row_cache_size = (cache_size_gb*(1024*1024*1024))/
                ((double)sizeof(double)*NUM_COLS);
            BOOST_LOG_TRIVIAL(info) << "Cache size: " << cache_size_gb
                << "GB, #Rows: " << g_row_cache_size;

            g_cache_update_iter = rc_update_start_interval;

            g_row_cache = partition_cache<double>::create(g_nthread,
                    NUM_COLS, g_row_cache_size/(g_nthread*2), g_row_cache_size);
        } else {
            BOOST_LOG_TRIVIAL(info) << "\n[INFO]: Row Cache inactive ...";
        }

        /*printf("Malloc-ing the whole dataset!\n");
          g_data.resize(NUM_ROWS);*/
        // End caching

        g_clusters = prune_clusters::create(K, NUM_COLS);
        if (centers)
            g_clusters->set_mean(*centers);

        FG_vector<unsigned>::ptr cluster_assignments; // Which cluster a sample is in
        std::vector<unsigned> num_members_v(K);

        BOOST_LOG_TRIVIAL(info) << "Init of g_cluster_dist";
        // Distance to everyone other than yourself
        g_cluster_dist = prune::dist_matrix::create(K);
        /*** End VarInit ***/

        if (!centers) {
            g_stage = INIT;

            if (init == "random") {
                BOOST_LOG_TRIVIAL(info) << "Running init: '"<< init <<"' ...";
                g_init = RANDOM;

                mat->start_all(vertex_initializer::ptr(),
                        vertex_program_creater::ptr(new kmeans_vertex_program_creater()));
                mat->wait4complete();

                if (g_row_cache)
                    manage_cache();

                g_io_reqs += NUM_ROWS;
                update_clusters(mat, num_members_v);
            }
            if (init == "forgy") {
                BOOST_LOG_TRIVIAL(info) << "Deterministic Init is: '"<< init <<"'";
                g_init = FORGY;

                // Select K in range NUM_ROWS
                std::vector<vertex_id_t> init_ids; // Used to start engine
                for (unsigned cl = 0; cl < K; cl++) {
                    vertex_id_t id = random() % NUM_ROWS;
                    g_init_hash[id] = cl; // <vertex_id, cluster_id>
                    init_ids.push_back(id);
                }
                mat->start(&init_ids.front(), K);
                mat->wait4complete();
                g_io_reqs++;
            } else if (init == "kmeanspp") {
                BOOST_LOG_TRIVIAL(info) << "Init is '"<< init <<"'";
                g_init = PLUSPLUS;

                // Init g_kmspp_distance to max distance
                g_kmspp_distance.assign(NUM_ROWS, std::numeric_limits<double>::max());

                g_kmspp_cluster_idx = 0;
                g_kmspp_next_cluster = random() % NUM_ROWS; // 0 - (NUM_ROWS - 1)

                BOOST_LOG_TRIVIAL(info) << "Assigning v:" << g_kmspp_next_cluster
                    << " as first cluster";
                g_kmspp_distance[g_kmspp_next_cluster] = 0;

                // Fire up K engines with 2 iters/engine
                while (true) {
                    // TODO: Start 1 vertex which will activate all
                    g_kmspp_stage = ADDMEAN;

                    mat->start(&g_kmspp_next_cluster, 1);
                    mat->wait4complete();

                    // Compute distance matrix
                     g_cluster_dist->compute_dist(g_clusters, NUM_COLS);
#if VERBOSE
                    BOOST_LOG_TRIVIAL(info) << "Printing clusters after sample set_mean ...";
                    g_clusters->print_means();
#endif
                    // skip distance comp since we picked clusters
                    if (g_kmspp_cluster_idx+1 == K) { break; }
                    g_kmspp_stage = DIST;

                    g_io_reqs += NUM_ROWS + 1;

                    BOOST_LOG_TRIVIAL(info) << "Entering DIST stage";
                    mat->start_all(vertex_initializer::ptr(),
                            vertex_program_creater::ptr(new kmeanspp_vertex_program_creater()));
                    mat->wait4complete();

                    if (g_row_cache)
                        manage_cache();
                    g_kmspp_next_cluster = kmeanspp_get_next_cluster_id(mat);
                }
            }
        } else
            g_clusters->print_means();

#if KM_TEST
        g_gb_obt_iter.push_back(mat->get_tot_bytes());
        g_cache_hits_iter.push_back(g_row_cache->get_cache_hits());
#endif
        gettimeofday(&start , NULL);

        if (init == "forgy" || init == "kmeanspp" || centers) {
            g_prune_init = true; // set
            g_stage = ESTEP;
            BOOST_LOG_TRIVIAL(info) << "Init: Computing cluster distance matrix ...";
            g_cluster_dist->compute_dist(g_clusters, NUM_COLS);
#if KM_TEST
            BOOST_LOG_TRIVIAL(info) << "Printing inited cluster distance matrix ...";
            g_cluster_dist->print();
#endif
            BOOST_LOG_TRIVIAL(info) << "Init: Running an engine for PRUNE since init is "
                << init;

            mat->start_all(vertex_initializer::ptr(),
                    vertex_program_creater::ptr(
                        new kmeans_vertex_program_creater()));
#if KM_TEST
            g_gb_obt_iter.push_back(mat->wait4complete());
            g_cache_hits_iter.push_back(g_row_cache->get_cache_hits());
#else
            mat->wait4complete();
#endif
            if (g_row_cache)
                manage_cache();

            BOOST_LOG_TRIVIAL(info) << "Init: M-step Updating cluster means ...";

            update_clusters(mat, num_members_v);
            g_io_reqs += NUM_ROWS;
#if KM_TEST
            BOOST_LOG_TRIVIAL(info) << "After Init engine: printing cluster counts:";
            print_vector<unsigned>(num_members_v);
            acntr->complete();
#endif

#if VERBOSE
            BOOST_LOG_TRIVIAL(info) << "After Init engine: clusters:";
            g_clusters->print_means();

            BOOST_LOG_TRIVIAL(info) << "After Init engine: cluster distance matrix ...";
            g_cluster_dist->compute_dist(g_clusters, NUM_COLS);
            g_cluster_dist->print();

            ac->init_iter();
#endif
            g_prune_init = false; // reset
            g_num_changed = 0; // reset
        }

        g_stage = ESTEP;
        BOOST_LOG_TRIVIAL(info) << "SEM-K||means starting ...";

        bool converged = false;

        std::string str_iters = max_iters == std::numeric_limits<unsigned>::max() ?
            "until convergence ...":
            std::to_string(max_iters) + " iterations ...";
        BOOST_LOG_TRIVIAL(info) << "Computing " << str_iters;
        g_iter = 1;

        while (g_iter < max_iters) {
            BOOST_LOG_TRIVIAL(info) << "E-step Iteration " << g_iter <<
                " . Computing cluster assignments ...";
            BOOST_LOG_TRIVIAL(info) << "Main: Computing cluster distance matrix ...";
            g_cluster_dist->compute_dist(g_clusters, NUM_COLS);

#if VERBOSE
            BOOST_LOG_TRIVIAL(info) << "Before: Cluster distance matrix ...";
            g_cluster_dist->print();
#endif
            mat->start_all(vertex_initializer::ptr(),
                    vertex_program_creater::ptr(new kmeans_vertex_program_creater()));
#if KM_TEST
            g_gb_obt_iter.push_back(mat->wait4complete());
            g_cache_hits_iter.push_back(g_row_cache->get_cache_hits());
            acntr->complete();
#else
            mat->wait4complete();
#endif

            if (g_row_cache)
                manage_cache();

            BOOST_LOG_TRIVIAL(info) << "Main: M-step Updating cluster means ...";
            update_clusters(mat, num_members_v);

#if VERBOSE
            BOOST_LOG_TRIVIAL(info) << "Getting cluster membership ...";
            get_membership(mat)->print(NUM_ROWS);
            BOOST_LOG_TRIVIAL(info) << "Before: Printing Clusters:";
            g_clusters->print_means();
#endif

            BOOST_LOG_TRIVIAL(info) << "Printing cluster counts ...";
            print_vector<unsigned>(num_members_v);

            BOOST_LOG_TRIVIAL(info) << "** Samples changes cluster: "
                << g_num_changed << " **\n";

            if (g_num_changed == 0 || ((g_num_changed/(double)NUM_ROWS)) <= tolerance) {
                converged = true;
                break;
            } else {
                g_num_changed = 0;
            }
            g_iter++;

#if KM_TEST
            g_prune_stats->finalize();
#endif
#if VERBOSE
            ac->init_iter();
#endif
        }

        gettimeofday(&end, NULL);
        BOOST_LOG_TRIVIAL(info) << "\n\nAlgorithmic time taken = " <<
            time_diff(start, end) << " sec\n";
#if KM_TEST
        g_prune_stats->get_stats();
        BOOST_LOG_TRIVIAL(info) << "\nGBytes requested per iteration: ";
        print_vector<double>(g_gb_req_iter, 200);

        std::vector<double> v = per_iter_from_agg_io(g_gb_obt_iter);
        BOOST_LOG_TRIVIAL(info) << "\nGBytes obtained per iteration: ";
        print_vector<double>(v, 200);

        std::vector<size_t> cv = per_iter_from_agg_cache(g_cache_hits_iter);
        BOOST_LOG_TRIVIAL(info) << "\nRow-Cache hits per iteration: ";
        print_vector<size_t>(cv, 200);

        BOOST_LOG_TRIVIAL(info) << "\nActive count per iteration: ";
        print_vector<unsigned>(acntr->get_active_count_per_iter(), 200);
#endif
#if VERBOSE
        ac->write_consolidated("consol_activation_by_iter.csv", NUM_ROWS);
#endif

#ifdef PROFILER
        ProfilerStop();
#endif
        BOOST_LOG_TRIVIAL(info) << "\n******************************************\n";
        printf("Total # of IO requests: %lu\nTotal bytes requested: %lu\n",
                g_io_reqs, (g_io_reqs*(sizeof(double))*NUM_COLS));
        printf("# of Row Cache hits = %lu\n\n",
                g_row_cache->get_cache_hits());

        if (converged) {
            BOOST_LOG_TRIVIAL(info) <<
                "K-means converged in " << g_iter << " iterations";
        } else {
            BOOST_LOG_TRIVIAL(warning) << "[Warning]: K-means failed to converge in "
                << g_iter << " iterations";
        }
        BOOST_LOG_TRIVIAL(info) << "\n******************************************\n";

        print_vector<unsigned>(num_members_v);

        std::vector<std::vector<double>> means(K);
        copy_means(means);

        cluster_assignments = get_membership(mat);
        return sem_kmeans_ret::create(cluster_assignments, means, num_members_v, g_iter);
    }
}
