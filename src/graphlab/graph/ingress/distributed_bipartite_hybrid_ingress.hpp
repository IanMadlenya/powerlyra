

#ifndef GRAPHLAB_DISTRIBUTED_BIPARTITE_HYBRID_INGRESS_HPP
#define GRAPHLAB_DISTRIBUTED_BIPARTITE_HYBRID_INGRESS_HPP

#include <boost/functional/hash.hpp>

#include <graphlab/rpc/buffered_exchange.hpp>
#include <graphlab/graph/graph_basic_types.hpp>
#include <graphlab/graph/ingress/distributed_ingress_base.hpp>
#include <graphlab/graph/distributed_graph.hpp>
#include <graphlab/graph/ingress/sharding_constraint.hpp>
#include <graphlab/graph/ingress/ingress_edge_decision.hpp>


#include <graphlab/macros_def.hpp>
#include <map>
#include <set>
#include <vector>

namespace graphlab {
  template<typename VertexData, typename EdgeData>
  class distributed_graph;

  /**
   * \brief Ingress object assigning edges using randoming hash function.
   */
  template<typename VertexData, typename EdgeData>
  class distributed_bipartite_hybrid_ingress : 
    public distributed_ingress_base<VertexData, EdgeData> {
  public:
    typedef distributed_graph<VertexData, EdgeData> graph_type;
    /// The type of the vertex data stored in the graph 
    typedef VertexData vertex_data_type;
    /// The type of the edge data stored in the graph 
    typedef EdgeData edge_data_type;


    typedef distributed_ingress_base<VertexData, EdgeData> base_type;


    typedef typename graph_type::vertex_record vertex_record;
    
    typedef typename base_type::edge_buffer_record edge_buffer_record;
    typedef typename buffered_exchange<edge_buffer_record>::buffer_type 
        edge_buffer_type;
    typedef typename base_type::vertex_buffer_record vertex_buffer_record;
    typedef typename buffered_exchange<vertex_buffer_record>::buffer_type 
        vertex_buffer_type;
    typedef typename std::pair<edge_buffer_record,int> edge_msg;
    typedef typename buffered_exchange<edge_msg>::buffer_type 
        edge_msg_type;
    
    
    graph_type& graph;
    dc_dist_object<distributed_bipartite_hybrid_ingress> hybrid_rpc;

    buffered_exchange<vertex_buffer_record> hybrid_vertex_exchange;
    buffered_exchange<edge_buffer_record> hybrid_edge_exchange;
    buffered_exchange<edge_msg> hybrid_msg_exchange;
    
    
    std::vector<edge_buffer_record> hybrid_edges;

    bool source_is_special;



  public:
    distributed_bipartite_hybrid_ingress(distributed_control& dc, graph_type& graph,const std::string& specialvertex) :
      base_type(dc, graph),graph(graph),hybrid_rpc(dc, this),
      hybrid_vertex_exchange(dc),hybrid_edge_exchange(dc),hybrid_msg_exchange(dc)
    {

      if(specialvertex=="source")
        source_is_special=true;
      else
        source_is_special=false;         
      
    } // end of constructor

    ~distributed_bipartite_hybrid_ingress() { 
      
    }

    /** Add an edge to the ingress object using random assignment. */
    void add_edge(vertex_id_type source, vertex_id_type target,
                  const EdgeData& edata) {
      const edge_buffer_record record(source, target, edata);
      if(source_is_special){  
        const procid_t owning_proc = 
          graph_hash::hash_vertex(target) % hybrid_rpc.numprocs();
        hybrid_edge_exchange.send(owning_proc, record);
      }
      else{
        const procid_t owning_proc = 
          graph_hash::hash_vertex(source) % hybrid_rpc.numprocs();
        hybrid_edge_exchange.send(owning_proc, record);
      }
    } // end of add edge
    
    void add_vertex(vertex_id_type vid, const VertexData& vdata) { 
      const vertex_buffer_record record(vid, vdata);
      const procid_t owning_proc = 
        graph_hash::hash_vertex(vid) % hybrid_rpc.numprocs();        
      hybrid_vertex_exchange.send(owning_proc, record);
    } // end of add vertex

    void finalize() {
      

      edge_buffer_type edge_buffer;
      
      hopscotch_map<vertex_id_type, size_t> in_degree_set;
      procid_t proc;

      hybrid_edge_exchange.flush();
      hybrid_vertex_exchange.flush();
      
      {
        size_t changed_size = hybrid_edge_exchange.size() + hybrid_vertex_exchange.size();
        hybrid_rpc.all_reduce(changed_size);
        if (changed_size == 0) {
          logstream(LOG_INFO) << "Skipping Graph Finalization because no changes happened..." << std::endl;
          return;
        }
      }

      proc = -1;
      while(hybrid_edge_exchange.recv(proc, edge_buffer)) {
        foreach(const edge_buffer_record& rec, edge_buffer) {
          hybrid_edges.push_back(rec);
          if(source_is_special)
            in_degree_set[rec.target]++;
          else
            in_degree_set[rec.source]++;
        }
      }
      hybrid_edge_exchange.clear();
      hybrid_edge_exchange.barrier(); // sync before reusing

      // re-send edges of high-degree vertices
      for (size_t i = 0; i < hybrid_edges.size(); i++) {
        edge_buffer_record& rec = hybrid_edges[i];
        if(source_is_special){
          const procid_t owner_proc = 
            graph_hash::hash_vertex(rec.source) % hybrid_rpc.numprocs();
          hybrid_msg_exchange.send(owner_proc,std::make_pair(rec,in_degree_set[rec.target]));
        }
        else{
          const procid_t owner_proc = 
            graph_hash::hash_vertex(rec.target) % hybrid_rpc.numprocs();
          hybrid_msg_exchange.send(owner_proc,std::make_pair(rec,in_degree_set[rec.source]));
        }
      }
      in_degree_set.clear();
      hybrid_edges.clear();
      hybrid_msg_exchange.flush();
      std::map<vertex_id_type,std::vector<float> > count_map;
      std::map<vertex_id_type,procid_t > owner_map;

      proc = -1;
      
      edge_msg_type msg_buffer;
      if(source_is_special){
        while(hybrid_msg_exchange.recv(proc, msg_buffer)) {
          foreach(const edge_msg& msg, msg_buffer) {
            
            hybrid_edges.push_back(msg.first);
            if(count_map.find(msg.first.source)==count_map.end()){
              count_map[msg.first.source].resize(hybrid_rpc.numprocs());
              owner_map[msg.first.source]=hybrid_rpc.procid();
            }
            const procid_t target_proc = 
              graph_hash::hash_vertex(msg.first.target) % hybrid_rpc.numprocs();
            //count_map[msg.first.source][target_proc]+=1.0/(msg.second+1);
            count_map[msg.first.source][target_proc]+=1.0;///(msg.second+1);
            const procid_t max_proc    = owner_map[msg.first.source];
            if(count_map[msg.first.source][target_proc] > count_map[msg.first.source][max_proc])
              owner_map[msg.first.source]=target_proc;
          }
        }
      }
      else{
        while(hybrid_msg_exchange.recv(proc, msg_buffer)) {
          foreach(const edge_msg& msg, msg_buffer) {
            
            hybrid_edges.push_back(msg.first);
            if(count_map.find(msg.first.target)==count_map.end()){
              count_map[msg.first.target].resize(hybrid_rpc.numprocs());
              owner_map[msg.first.target]=hybrid_rpc.procid();
            }
            const procid_t source_proc = 
              graph_hash::hash_vertex(msg.first.source) % hybrid_rpc.numprocs();
            count_map[msg.first.target][source_proc]+=1.0;///(msg.second+1);
            const procid_t max_proc    = owner_map[msg.first.target];
            if(count_map[msg.first.target][source_proc] > count_map[msg.first.target][max_proc])
              owner_map[msg.first.target]=source_proc;
          }
        }
      }

      // re-send 
      for (size_t i = 0; i < hybrid_edges.size(); i++) {
        edge_buffer_record& rec = hybrid_edges[i];
        procid_t owner_proc ;
        if(source_is_special)
          owner_proc = owner_map[rec.source];
        else
          owner_proc = owner_map[rec.target];
        hybrid_edge_exchange.send(owner_proc,rec);   
      }
      hybrid_edge_exchange.flush();
      
      std::set<vertex_id_type> master_set;
      proc = -1;
      while(hybrid_edge_exchange.recv(proc, edge_buffer)) {
        foreach(const edge_buffer_record& rec, edge_buffer) {
          //hybrid_edges.push_back(rec);
          if(source_is_special)
            master_set.insert(rec.source);
          else
            master_set.insert(rec.target);
          base_type::edge_exchange.send(hybrid_rpc.procid(), rec);
        }
      }
      hybrid_edge_exchange.clear();

      graph.lvid2record.resize(master_set.size());
      graph.local_graph.resize(master_set.size());
      lvid_type lvid = 0;
      foreach(const vertex_id_type& vid,master_set){
        graph.vid2lvid[vid]=lvid;
        //graph.local_graph.add_vertex(lvid,vdata());
        vertex_record& vrec = graph.lvid2record[lvid];
        vrec.gvid = vid;
        vrec.owner=hybrid_rpc.procid();
        lvid++;
      }

      { // Receive any vertex data sent by other machines
        vertex_buffer_type vertex_buffer; procid_t sending_proc(-1);
        while(hybrid_vertex_exchange.recv(sending_proc, vertex_buffer)) {
          foreach(const vertex_buffer_record& rec, vertex_buffer) {
            if(owner_map.find(rec.vid) !=owner_map.end()){
              base_type::vertex_exchange.send(owner_map[rec.vid],rec);
            }
            else{
              base_type::vertex_exchange.send(hybrid_rpc.procid(),rec);
            }
          }
        }
        hybrid_vertex_exchange.clear();
      } // end of loop to populate vrecmap

      base_type::finalize();
    } // end of finalize

  }; // end of distributed_bipartite_hybrid_ingress
}; // end of namespace graphlab
#include <graphlab/macros_undef.hpp>


#endif
