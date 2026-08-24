#pragma once

#include <json/json.h>

#include <cstdint>
#include <string>
#include <vector>

#include "domain/AuditEntry.h"

namespace launcher::repositories::postgres {

/// Conventions shared by every PostgreSQL repository.
///
/// uuid columns are compared against `$n::uuid` rather than the bare parameter: libpq sends
/// parameters as text, and PostgreSQL will not implicitly cast text to uuid in a comparison.
///
/// Timestamps leave the database already formatted as ISO 8601 in UTC, so this layer stays the
/// only one that has to know how PostgreSQL represents a timestamptz.

/// Binds a number as text, never as a C++ integer.
///
/// Drogon sends an integral parameter in PostgreSQL's *binary* format, sized by the C++ type,
/// so an `int` reaching a `bigint` column is rejected as malformed binary input. A text
/// parameter is parsed by the server into whatever type it inferred for that position, which
/// is correct whatever the column happens to be.
inline std::string number(int64_t value) {
    return std::to_string(value);
}

inline std::string toCompactJson(const Json::Value& value) {
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    return Json::writeString(builder, value);
}

/// A list of values as one `jsonb` parameter, expanded server-side with
/// `jsonb_array_elements`. A PostgreSQL array literal would mean hand-rolling the
/// array-literal escaping rules for paths and hashes; jsoncpp already escapes correctly.
inline std::string jsonArrayParameter(const std::vector<std::string>& values) {
    Json::Value array(Json::arrayValue);
    for (const auto& value : values) {
        array.append(value);
    }
    return toCompactJson(array);
}

/// The `metadata` column of an audit entry, as a real jsonb object rather than a string of JSON.
/// Shared by every repository that writes the trail, so the one field whose shape varies by
/// action cannot vary by writer as well.
inline std::string auditMetadataJson(const std::vector<domain::AuditField>& fields) {
    Json::Value object(Json::objectValue);
    for (const auto& [name, value] : fields) {
        object[name] = value;
    }
    return toCompactJson(object);
}

} // namespace launcher::repositories::postgres
