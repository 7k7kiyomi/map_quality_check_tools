#pragma once

#include <cstddef>
#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace LineTopologyIssueAggregation {

struct Issue {
    std::string sourceFile;
    std::int64_t featureId = -1;
    std::string errorType;
    std::string description;
};

struct IssueGroup {
    std::string sourceFile;
    std::int64_t featureId = -1;
    std::vector<std::size_t> memberIndices;
    std::vector<std::string> errorTypes;
    std::vector<std::string> descriptions;
};

inline void appendUnique(std::vector<std::string>& values, const std::string& value)
{
    if (value.empty()) return;
    for (const std::string& existing : values) {
        if (existing == value) return;
    }
    values.push_back(value);
}

inline std::string canonicalErrorType(const std::string& value)
{
    if (value == u8"河网通达性-悬挂") return u8"河流悬挂";
    if (value == u8"河网通达性-独立") return u8"河流独立";
    return value;
}

inline std::string canonicalDescription(const std::string& value)
{
    const std::string prefix = u8"河网通达性检查：";
    return value.compare(0, prefix.size(), prefix) == 0 ? value.substr(prefix.size()) : value;
}

// DBF strings are limited to bytes, so never split inside a UTF-8 character.
inline std::vector<std::string> splitUtf8(const std::string& text, std::size_t limit = 254)
{
    if (text.empty() || limit == 0) return {text};
    std::vector<std::string> parts;
    for (std::size_t start = 0; start < text.size();) {
        std::size_t end = start + std::min(limit, text.size() - start);
        while (end < text.size() && end > start
               && (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80) --end;
        if (end == start) {
            end = start + 1;
            while (end < text.size() && (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80) ++end;
        }
        parts.push_back(text.substr(start, end - start));
        start = end;
    }
    return parts;
}

inline std::vector<IssueGroup> groupBySourceFeature(const std::vector<Issue>& issues)
{
    std::vector<IssueGroup> groups;
    std::map<std::pair<std::string, std::int64_t>, std::size_t> groupByKey;

    for (std::size_t i = 0; i < issues.size(); ++i) {
        const Issue& issue = issues[i];
        const bool hasStableIdentity = !issue.sourceFile.empty() && issue.featureId >= 0;
        const std::pair<std::string, std::int64_t> key = hasStableIdentity
            ? std::make_pair(issue.sourceFile, issue.featureId)
            : std::make_pair(std::string(), static_cast<std::int64_t>(i));

        auto found = groupByKey.find(key);
        if (found == groupByKey.end()) {
            IssueGroup group;
            group.sourceFile = issue.sourceFile;
            group.featureId = issue.featureId;
            group.memberIndices.push_back(i);
            appendUnique(group.errorTypes, canonicalErrorType(issue.errorType));
            appendUnique(group.descriptions, canonicalDescription(issue.description));
            groupByKey.emplace(key, groups.size());
            groups.push_back(std::move(group));
            continue;
        }

        IssueGroup& group = groups[found->second];
        group.memberIndices.push_back(i);
        appendUnique(group.errorTypes, canonicalErrorType(issue.errorType));
        appendUnique(group.descriptions, canonicalDescription(issue.description));
    }

    return groups;
}

} // namespace LineTopologyIssueAggregation
