#ifndef NDNSF_DISTRIBUTED_REPO_DETAIL_STRICT_JSON_HPP
#define NDNSF_DISTRIBUTED_REPO_DETAIL_STRICT_JSON_HPP

#include <ndn-service-framework/strict-json.hpp>

namespace ndnsf_distributed_repo::detail {

// Preserve the existing Repo codec interface while sharing the maintained parser.
using ndn_service_framework::json::JsonValue;
using ndn_service_framework::json::JsonParser;
using ndn_service_framework::json::isValidUtf8;
using ndn_service_framework::json::appendUtf8;
using ndn_service_framework::json::parseJson;
using ndn_service_framework::json::requireObject;
using ndn_service_framework::json::requireExactKeys;
using ndn_service_framework::json::member;
using ndn_service_framework::json::quoteJson;
using ndn_service_framework::json::renderJson;

} // namespace ndnsf_distributed_repo::detail

#endif // NDNSF_DISTRIBUTED_REPO_DETAIL_STRICT_JSON_HPP
