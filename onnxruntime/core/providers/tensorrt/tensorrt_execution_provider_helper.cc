// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "core/providers/shared_library/provider_api.h"
#include "tensorrt_execution_provider.h"
#include "core/framework/murmurhash3.h"
#include <iostream>

namespace onnxruntime {

namespace {
// Get unique graph name based on graph's name and all nodes' name
std::string GetUniqueGraphName(const Graph& graph) {
  HashValue model_hash = 0;
  uint32_t hash[4] = {0, 0, 0, 0};

  auto hash_str = [&hash](const std::string& str) {
    MurmurHash3::x86_128(str.data(), str.size(), hash[0], &hash);
  };

  // Hash all nodes' name
  for (int i = 0; i < graph.MaxNodeIndex(); ++i) {
    auto node = graph.GetNode(i);
    if (node == nullptr) {
      continue;
    }
    hash_str(node->Name());
  }

  model_hash = hash[0] | (uint64_t(hash[1]) << 32);

  return graph.Name() + "_" + std::to_string(model_hash);
}
}  // namespace

// The newly-built graph has not yet being resolved by Graph::Resolve(), so we can't leverage
// Graph::ResolveContext::IsInputInitializerOrOutput(). We have to implement this fuction again.
bool TensorrtExecutionProvider::IsInputInitializerOrOutput(const Graph& graph,
                                                           const std::string& name,
                                                           bool check_ancestors) const {
  const Graph* parent_graph = nullptr;
  return IsLocalValue(graph, name) ||
         (check_ancestors && (parent_graph = graph.ParentGraph()) != nullptr &&
          IsInputInitializerOrOutput(*parent_graph, name, check_ancestors));
}

// The newly-built graph has not yet being resolved by Graph::Resolve(), so we can't leverage
// Graph::ResolveContext::IsOuterScopeValue(). We have to implement this function again.
bool TensorrtExecutionProvider::IsOuterScopeValue(const Graph& graph,
                                                  const std::string& name) const {
  const Graph* parent_graph = nullptr;
  return (parent_graph = graph.ParentGraph()) != nullptr &&
         IsInputInitializerOrOutput(*parent_graph, name, true);
}

// The newly-built graph has not yet being resolved by Graph::Resolve(), so we can't leverage
// Graph::ResolveContext::IsLocalValue(). We have to implement this function again.
bool TensorrtExecutionProvider::IsLocalValue(const Graph& graph,
                                             const std::string& name) const {
  std::string unique_graph_name = GetUniqueGraphName(graph);
  if (subgraph_context_map_.find(unique_graph_name) == subgraph_context_map_.end()) {
    return false;
  }
  SubGraphContext* context = subgraph_context_map_.at(unique_graph_name).get();
  return context->output_args.find(name) != context->output_args.cend() ||
         context->inputs_and_initializers.find(name) != context->inputs_and_initializers.cend();
}

/**
 * Set inputs, initializers and outputs for all subgraphs during TensorrtExecutionProvider::GetSupportedList()
 * and save those information in subgraph context data structure. It's useful for building a valid graph and
 * make Graph::Resolve() happy especially when dealing with nested control-flow op graph.
 */
void TensorrtExecutionProvider::BuildSubGraphContext(Graph& graph) const {
  // Iterate all the nodes and recurse into inner most subgraph first
  for (int i = 0; i < graph.MaxNodeIndex(); ++i) {
    auto node = graph.GetNode(i);
    if (node == nullptr) {
      continue;
    }

    auto& subgraph_map = node->GetAttributeNameToMutableSubgraphMap();
    for (auto& entry : subgraph_map) {
      Graph* subgraph = entry.second;
      BuildSubGraphContext(*subgraph);
    }
  }

  std::string unique_graph_name = GetUniqueGraphName(graph);

  // Subgraph context has been built before, no need to do it again
  if (subgraph_context_map_.find(unique_graph_name) != subgraph_context_map_.end()) {
    return;
  }

  subgraph_context_map_.emplace(unique_graph_name, std::make_unique<SubGraphContext>());
  SubGraphContext* context = subgraph_context_map_.at(unique_graph_name).get();

  // Collect all nodes' outputs and nodes' name
  for (int i = 0; i < graph.MaxNodeIndex(); ++i) {
    auto node = graph.GetNode(i);
    if (node == nullptr) {
      continue;
    }

    for (const auto& output : node->OutputDefs()) {
      context->output_args.insert(output->Name());
    }
  }

  // Go thru all node's inputs
  for (int i = 0; i < graph.MaxNodeIndex(); ++i) {
    auto node = graph.GetNode(i);
    if (node == nullptr) {
      continue;
    }

    for (const auto& input : node->InputDefs()) {
      if (context->output_args.find(input->Name()) != context->output_args.end()) {
        continue;
      }
      // This input arg is not the output of another node so must come from either a graph input or an initializer.
      context->inputs_and_initializers[input->Name()] = input;
      ORT_THROW_IF_ERROR(graph_utils::ConvertInMemoryDataToInline(graph, input->Name()));
    }
  }
}

// Set outer scope values for subgraphs and add thoes values as top-level graph's inputs if needed.
void TensorrtExecutionProvider::SetGraphOuterScopeValuesAndInputs(Graph& graph_build,
                                                                  const Graph& graph) const {
  // Iterate all the nodes and recurse into inner most subgraph first for both newly built graph and original graph
  for (int i = 0; i < graph_build.MaxNodeIndex(); ++i) {
    auto graph_build_node = graph_build.GetNode(i);
    if (graph_build_node == nullptr) {
      continue;
    }

    auto graph_build_map = graph_build_node->GetAttributeNameToMutableSubgraphMap();
    std::unordered_map<std::string, gsl::not_null<const Graph*>> subgraph_map;
    const Node* graph_node = nullptr;

    // Find corresponding original graph node's subgraphs
    for (int j = 0; j < graph.MaxNodeIndex(); ++j) {
      if (graph.GetNode(j) && graph.GetNode(j)->Name() == graph_build_node->Name()) {
        graph_node = graph.GetNode(j);
        subgraph_map = graph_node->GetAttributeNameToSubgraphMap();
        break;
      }
    }

    for (auto& entry : graph_build_map) {
      auto attr_name = entry.first;
      Graph* subgraph_build = entry.second;
      if (subgraph_map.find(attr_name) != subgraph_map.end()) {
        // recurse into subgraph
        const Graph* subgraph = subgraph_map.at(attr_name);
        SetGraphOuterScopeValuesAndInputs(*subgraph_build, *subgraph);
      }
    }
  }

  // Start from the inner most subgraph first and check whether its outer scope values are existed in the
  // newly built graph. If not, we need to add those outer scope values as explicit inputs to the top-level
  // of newly built graph.
  if (graph_build.ParentNode()) {
    auto top_level_graph = &graph_build;
    while (top_level_graph->MutableParentGraph()) {
      top_level_graph = top_level_graph->MutableParentGraph();
    }
    std::string unique_graph_name = GetUniqueGraphName(*top_level_graph);
    if (subgraph_context_map_.find(unique_graph_name) == subgraph_context_map_.end()) {
      LOGS_DEFAULT(ERROR) << "[TensorRT EP] Can't find top-level graph context. \
                              Please check BuildSubGraphContext() has built the graph context correctly.";
      return;
    }

    SubGraphContext* context = subgraph_context_map_.at(unique_graph_name).get();

    LOGS_DEFAULT(VERBOSE) << "[TensorRT EP] Subgraph name is " << graph_build.Name();
    LOGS_DEFAULT(VERBOSE) << "[TensorRT EP] Its parent node is " << graph.ParentNode()->Name();
    LOGS_DEFAULT(VERBOSE) << "[TensorRT EP] Its parent node's implicit inputs:";

    // Iterate all the implicit inputs to set outer scope value for the newly built subgraph
    for (const auto& input : graph.ParentNode()->ImplicitInputDefs()) {
      LOGS_DEFAULT(VERBOSE) << "[TensorRT EP] \t" << input->Name();

      // The node arg in parent node's implicit inputs could be used for parent node's other subgraph, for example
      // "If" op has two subgraphs. So we need to make sure that the node arg is used in current subgraph only.
      // (GetNodeArg searches for specific node arg in all node args in the graph)
      if (graph_build.GetNodeArg(input->Name())) {
        graph_build.AddOuterScopeNodeArg(input->Name());
        LOGS_DEFAULT(VERBOSE) << "[TensorRT EP] \t" << input->Name() << " is used in this subgraph";

        if (context &&
            (context->manually_added_graph_inputs.find(input->Name()) != context->manually_added_graph_inputs.end())) {
          LOGS_DEFAULT(VERBOSE) << "[TensorRT EP] \t" << input->Name() << " is already been added as an explicit input to graph";
          continue;
        }

        // Handle the case where this outer scope value is not existed in any outer scope levels of the
        // newly built graph (the newly built graph is the subgraph of the original graph). Need to add
        // the outer scope value as an explicit input to the top-level of newly built graph.
        if (!IsOuterScopeValue(graph_build, input->Name())) {
          const auto& name = input->Name();
          auto graph_inputs_including_initializers = top_level_graph->GetInputsIncludingInitializers();
          auto added_graph_input = std::find_if(graph_inputs_including_initializers.begin(),
                                                graph_inputs_including_initializers.end(),
                                                [&name](const NodeArg* entry) { return entry->Name() == name; });

          if (added_graph_input == graph_inputs_including_initializers.end()) {
            if (context) {
              auto type_proto = ONNX_NAMESPACE::TypeProto::Create();
              type_proto->copy_from(input->TypeAsProto());
              auto& n_input = top_level_graph->GetOrCreateNodeArg(name, type_proto.get());
              context->manually_added_graph_inputs[n_input.Name()] = &n_input;
              LOGS_DEFAULT(VERBOSE) << "[TensorRT EP] \t" << n_input.Name() << " is added as an explicit input into the newly built graph";
            }
          }
        }
      }
    }
  }
}

// If ORT TRT manually sets graph input in TensorrtExecutionProvider::SetGraphOuterScopeValuesAndInputs(),
// we have to manully set all the graph inputs in order to pass Graph::Resolve()
void TensorrtExecutionProvider::SetAllGraphInputs(Graph& graph) const {
  // If ORT TRT doesn't manully set graph input in TensorrtExecutionProvider::SetGraphOuterScopeValuesAndInputs(),
  // Graph::Resolve() will help set graph inputs in Graph::SetGraphInputsOutputs(), so no need to set graph inputs here.
  std::string unique_graph_name = GetUniqueGraphName(graph);
  if (subgraph_context_map_.find(unique_graph_name) == subgraph_context_map_.end() ||
      subgraph_context_map_[unique_graph_name].get()->manually_added_graph_inputs.size() == 0) {
    return;
  }

  SubGraphContext* context = subgraph_context_map_[unique_graph_name].get();
  std::vector<const NodeArg*> graph_inputs_including_initializers;
  std::unordered_set<std::string> graph_inputs_including_initializers_set;

  for (const auto& entry : context->inputs_and_initializers) {
    graph_inputs_including_initializers.push_back(entry.second);
    graph_inputs_including_initializers_set.insert(entry.first);
  }

  for (const auto& entry : context->manually_added_graph_inputs) {
    if (graph_inputs_including_initializers_set.find(entry.first) == graph_inputs_including_initializers_set.end()) {
      graph_inputs_including_initializers.push_back(entry.second);
      graph_inputs_including_initializers_set.insert(entry.first);
    }
  }

  for (const auto& node_arg : graph.GetInputsIncludingInitializers()) {
    if (graph_inputs_including_initializers_set.find(node_arg->Name()) == graph_inputs_including_initializers_set.end()) {
      graph_inputs_including_initializers.push_back(node_arg);
      graph_inputs_including_initializers_set.insert(node_arg->Name());
    }
  }

  graph.SetInputs(graph_inputs_including_initializers);
}

/**
 *  This is the helper function for ConstantFoldingDQ graph transformer.
 *
 *  It selects the qualified/required DQ node to be optimized as well as provides a mapping table
 *  to help TRT EP later include the DQ node which is filtered out by TRT parser.
 */
void TensorrtExecutionProvider::SelectQualifiedDQNode(const GraphViewer& graph,
                                                      std::unordered_set<NodeIndex>& selection_node_set,
                                                      std::unordered_map<NodeIndex, NodeIndex>& consumer_to_dq) const {
  LOGS_DEFAULT(VERBOSE) << "[TensorRT EP] Select qualified DQ nodes ...";
  const std::vector<NodeIndex>& node_index = graph.GetNodesInTopologicalOrder(1 /*priority-based topological sort*/);
  for (auto index : node_index) {
    auto* node = graph.GetNode(index);
    if (!node) {
      continue;
    }

    const auto* input_def = node->InputDefs()[0];  // Get NodeArg of the initializer of the DequantizeLinear node;
    auto data_type = input_def->TypeAsProto()->tensor_type().elem_type();
    auto constant_initializer = graph.IsConstantInitializer(input_def->Name(), true);

    // Node selection: (i.e. initializer -> DQ -> bias of X)
    // 1. DequantizeLinear op
    // 2. DQ node does not produce graph output, single consumer
    // 3. The first input of DQ is constant initializer.
    // 4. The data type of initializer is INT32, UINT16 or INT16
    // 5. X should be Gemm, Conv or LayerNormalization ?
    if (node->OpType() == "DequantizeLinear" &&
        node->GetOutputEdgesCount() == 1 &&
        (data_type == ONNX_NAMESPACE::TensorProto_DataType_INT32 || data_type == ONNX_NAMESPACE::TensorProto_DataType_INT16 || data_type == ONNX_NAMESPACE::TensorProto_DataType_UINT16) &&
        constant_initializer) {
      const Node& consumer_node = *node->OutputNodesBegin();
      selection_node_set.insert(index);
      consumer_to_dq[consumer_node.Index()] = index;
      LOGS_DEFAULT(VERBOSE) << "[TensorRT EP] " << consumer_node.Name() << " <- " << node->Name();
    }
  }
  LOGS_DEFAULT(VERBOSE) << "[TensorRT EP] Total " << selection_node_set.size() << " DequantizeLinear node(s) are selected.";
}

/**
 * This function returns an optimization ComputeCapability that is limited to:
 *  1. the DQ nodes in this individual TRT ComputeCapability
 *  2. the DQ nodes that are qualified and selected by TRT EP
 *
 * It also needs to make sure the DQ nodes is a subset of the complete list of DQ nodes to optimize in original selection ComputeCapability.
 * Finally, copy the optimization function from the original selection ComputeCapability.
 */
std::unique_ptr<ComputeCapability> TensorrtExecutionProvider::CreateOptimizationComputeCapability(ComputeCapability* selection_cc,
                                                                                                  std::unordered_set<NodeIndex>& trt_selection_node_set,
                                                                                                  ComputeCapability* trt_cc) const {
  auto sub_graph = onnxruntime::IndexedSubGraph::Create();
  std::unordered_set<NodeIndex> selection_node_set;

  for (auto index : selection_cc->SubGraph()->Nodes()) {
    selection_node_set.insert(index);
  }

  for (auto index : trt_cc->SubGraph()->Nodes()) {
    if (selection_node_set.find(index) == selection_node_set.end()) {
      continue;
    }
    if (trt_selection_node_set.find(index) == trt_selection_node_set.end()) {
      continue;
    }
    sub_graph->Nodes().push_back(index);
  }
  auto compute_capability = ComputeCapability::Create(std::move(sub_graph));
  compute_capability->copy_optimization_func(selection_cc);
  return compute_capability;
}

/**
 * This function helps add back the DQ nodes that are filtered out by TRT parser.
 * The reason is the DQ nodes can be optimized and dequantized by applying ConstantFoldingDQ optimizer by ORT L2+ optimization.
 */
void TensorrtExecutionProvider::UpdateSupportedNodeVectorForDQ(const GraphViewer& graph,
                                                               SubGraph_t& supported_node_vector,
                                                               SubGraphCollection_t& supported_nodes_vector,
                                                               std::unordered_map<NodeIndex, NodeIndex> consumer_to_dq) const {
  if (consumer_to_dq.empty()) {
    return;
  }

  if (!supported_node_vector.second) {
    return;
  }

  const std::vector<NodeIndex>& node_index = graph.GetNodesInTopologicalOrder(1);
  auto supported_nodes = supported_node_vector.first;
  for (auto index : supported_nodes) {
    if (consumer_to_dq.find(node_index[index]) == consumer_to_dq.end()) {
      continue;
    }

    auto dq_node_index = consumer_to_dq[node_index[index]];

    // Check if DQ node is included in one of the subgraphs
    auto in_the_subgraph_collection = [&](NodeIndex node_idx) -> bool {
      for (auto& node_vector : supported_nodes_vector) {
        if (!node_vector.second) {
          continue;
        }
        for (auto i : node_vector.first) {
          if (node_index[i] == node_idx) {
            return true;
          }
        }
      }
      return false;
    };

    // If the DQ node is already in the subgraph, do nothing.
    if (in_the_subgraph_collection(dq_node_index)) {
      continue;
    }

    // Find the iterator pointing to the target element
    auto it = std::find(node_index.begin(), node_index.end(), dq_node_index);
    if (it != node_index.end()) {
      // Calculate the index
      size_t idx = std::distance(node_index.begin(), it);
      supported_node_vector.first.push_back(idx);
      auto node = graph.GetNode(dq_node_index);
      LOGS_DEFAULT(VERBOSE) << "[TensorRT EP] " << node->Name() << " is included which is filtered out by TRT parser.";
    }
  }
}
}  // namespace onnxruntime
