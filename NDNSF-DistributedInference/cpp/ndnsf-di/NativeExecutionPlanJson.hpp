#ifndef NDNSF_DISTRIBUTED_INFERENCE_NATIVE_EXECUTION_PLAN_JSON_HPP
#define NDNSF_DISTRIBUTED_INFERENCE_NATIVE_EXECUTION_PLAN_JSON_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlan.hpp"

#include <boost/property_tree/ptree_fwd.hpp>

#include <istream>
#include <map>
#include <string>
#include <vector>

namespace ndnsf::di {

std::vector<std::string>
stringArrayFromJson(const boost::property_tree::ptree& node, const std::string& key);

std::map<std::string, NativeExecutionPlan>
nativeExecutionPlansByServiceFromJson(std::istream& input);

NativeExecutionPlan
nativeExecutionPlanForServiceFromJson(std::istream& input, const std::string& serviceName);

} // namespace ndnsf::di

#endif // NDNSF_DISTRIBUTED_INFERENCE_NATIVE_EXECUTION_PLAN_JSON_HPP
