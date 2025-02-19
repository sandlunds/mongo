/**
 *    Copyright (C) 2019-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/stages/collection_helpers.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/exec/trial_period_utils.h"
#include "mongo/db/query/interval_evaluation_tree.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/query/plan_yield_policy_sbe.h"
#include "mongo/db/query/sbe_stage_builder_helpers.h"
#include "mongo/db/query/shard_filterer_factory_interface.h"
#include "mongo/db/query/stage_builder.h"
#include "mongo/util/pair_map.h"

namespace mongo::stage_builder {
/**
 * Creates a new compilation environment and registers global values within the
 * new environment.
 */
std::unique_ptr<sbe::RuntimeEnvironment> makeRuntimeEnvironment(
    const CanonicalQuery& cq,
    OperationContext* opCtx,
    sbe::value::SlotIdGenerator* slotIdGenerator);

class PlanStageReqs;
class PlanStageSlots;
sbe::value::SlotVector getSlotsToForward(const PlanStageReqs& reqs,
                                         const PlanStageSlots& outputs,
                                         const sbe::value::SlotVector& exclude = sbe::makeSV());

/**
 * This function prepares the SBE tree for execution, such as attaching the OperationContext,
 * ensuring that the SBE tree is registered with the PlanYieldPolicySBE and populating the
 * "RuntimeEnvironment".
 *
 * The caller should pass true for 'preparingFromCache' if the SBE plan being prepared is being
 * recovered from the SBE plan cache.
 */
void prepareSlotBasedExecutableTree(OperationContext* opCtx,
                                    sbe::PlanStage* root,
                                    PlanStageData* data,
                                    const CanonicalQuery& cq,
                                    const MultipleCollectionAccessor& collections,
                                    PlanYieldPolicySBE* yieldPolicy,
                                    bool preparingFromCache = false);

class PlanStageReqs;

/**
 * The ParameterizedIndexScanSlots struct is used by SlotBasedStageBuilder while building the index
 * scan stage to return the slots that are registered in the runtime environment and will be
 * populated based on the index bounds.
 */
struct ParameterizedIndexScanSlots {
    // Holds the low and high key for the single interval index scan algorithm.
    struct SingleIntervalPlan {
        sbe::value::SlotId lowKey;
        sbe::value::SlotId highKey;
    };

    // Holds the slots for the generic index scan algorithm.
    struct GenericPlan {
        // Holds the value whether the generic or optimized index scan should be used.
        sbe::value::SlotId isGenericScan;

        // Holds the value of the IndexBounds used for the generic index scan algorithm.
        sbe::value::SlotId indexBounds;

        // Holds the value of an array of low and high keys for each interval.
        sbe::value::SlotId lowHighKeyIntervals;
    };

    // In the case that the parameterized plan will always consist of a single interval index scan,
    // this holds the SingleInterval struct. Otherwise, holds the necessary slots for a fully
    // generic parameterized index scan plan.
    stdx::variant<SingleIntervalPlan, GenericPlan> slots;
};

/**
 * The PlanStageSlots class is used by SlotBasedStageBuilder to return the output slots produced
 * after building a stage.
 */
class PlanStageSlots {
public:
    // The _slots map is capable of holding different "classes" of slots:
    // 1) kMeta slots are used to hold the current document (kResult), record ID (kRecordId), and
    //    various pieces of metadata.
    // 2) kField slots represent the values of top-level fields, or in some cases of dotted field
    //    paths (when we are getting the dotted field from a non-multikey index and we know no array
    //    traversal is needed). These slots hold the actual values of the fields / field paths (not
    //    the sort key or collation comparison key for the field).
    // 3) kKey slots represent the raw key value that comes from an ixscan / ixseek stage for a
    //    given field path. This raw key value can be used for sorting / comparison, but it is not
    //    always equal to the actual value of the field path (for example, if the key is coming from
    //    an index that has a non-simple collation).
    enum class Type {
        kMeta,
        kField,
        kKey,
    };

    using Name = std::pair<Type, StringData>;
    using OwnedName = std::pair<Type, std::string>;

    static constexpr auto kField = Type::kField;
    static constexpr auto kKey = Type::kKey;
    static constexpr auto kMeta = Type::kMeta;

    static constexpr Name kResult = {kMeta, "result"_sd};
    static constexpr Name kRecordId = {kMeta, "recordId"_sd};
    static constexpr Name kReturnKey = {kMeta, "returnKey"_sd};
    static constexpr Name kSnapshotId = {kMeta, "snapshotId"_sd};
    static constexpr Name kIndexId = {kMeta, "indexId"_sd};
    static constexpr Name kIndexKey = {kMeta, "indexKey"_sd};
    static constexpr Name kIndexKeyPattern = {kMeta, "indexKeyPattern"_sd};

    PlanStageSlots() = default;

    PlanStageSlots(const PlanStageReqs& reqs, sbe::value::SlotIdGenerator* slotIdGenerator);

    bool has(const Name& str) const {
        return _slots.count(str);
    }

    sbe::value::SlotId get(const Name& str) const {
        auto it = _slots.find(str);
        invariant(it != _slots.end());
        return it->second;
    }

    boost::optional<sbe::value::SlotId> getIfExists(const Name& str) const {
        if (auto it = _slots.find(str); it != _slots.end()) {
            return it->second;
        }
        return boost::none;
    }

    void set(const Name& str, sbe::value::SlotId slot) {
        _slots.insert_or_assign(str, slot);
    }

    void set(OwnedName str, sbe::value::SlotId slot) {
        _slots.insert_or_assign(std::move(str), slot);
    }

    void clear(const Name& str) {
        _slots.erase(str);
    }

    void clearAllFields() {
        for (auto it = _slots.begin(); it != _slots.end();) {
            if (it->first.first == kField) {
                _slots.erase(it++);
                continue;
            }
            ++it;
        }
    }

    /**
     * This method applies an action to some/all of the slots within this struct. For each slot in
     * this struct, the action is will be applied to the slot if (and only if) the corresponding
     * flag in 'reqs' is true.
     */
    inline void forEachSlot(const PlanStageReqs& reqs,
                            const std::function<void(sbe::value::SlotId)>& fn) const;

    inline void forEachSlot(const PlanStageReqs& reqs,
                            const std::function<void(sbe::value::SlotId, const Name&)>& fn) const;

    inline void clearNonRequiredSlots(const PlanStageReqs& reqs);

private:
    PairMap<Type, std::string, sbe::value::SlotId> _slots;
};

/**
 * The PlanStageReqs class is used by SlotBasedStageBuilder to represent the incoming requirements
 * and context when building a stage.
 */
class PlanStageReqs {
public:
    using Type = PlanStageSlots::Type;
    using Name = PlanStageSlots::Name;
    using OwnedName = std::pair<Type, std::string>;

    static constexpr auto kField = PlanStageSlots::Type::kField;
    static constexpr auto kKey = PlanStageSlots::Type::kKey;
    static constexpr auto kMeta = PlanStageSlots::Type::kMeta;

    PlanStageReqs copy() const {
        return *this;
    }

    bool has(const Name& str) const {
        auto it = _slots.find(str);
        return it != _slots.end() && it->second;
    }

    PlanStageReqs& set(const Name& str) {
        _slots.insert_or_assign(str, true);
        return *this;
    }

    PlanStageReqs& set(OwnedName str) {
        _slots.insert_or_assign(std::move(str), true);
        return *this;
    }

    PlanStageReqs& set(const std::vector<Name>& strs) {
        for (size_t i = 0; i < strs.size(); ++i) {
            _slots.insert_or_assign(strs[i], true);
        }
        return *this;
    }

    PlanStageReqs& set(std::vector<OwnedName> strs) {
        for (size_t i = 0; i < strs.size(); ++i) {
            _slots.insert_or_assign(std::move(strs[i]), true);
        }
        return *this;
    }

    PlanStageReqs& setIf(const Name& str, bool condition) {
        if (condition) {
            _slots.insert_or_assign(str, true);
        }
        return *this;
    }

    PlanStageReqs& setFields(std::vector<std::string> strs) {
        for (size_t i = 0; i < strs.size(); ++i) {
            _slots.insert_or_assign(std::make_pair(kField, std::move(strs[i])), true);
        }
        return *this;
    }

    PlanStageReqs& setKeys(std::vector<std::string> strs) {
        for (size_t i = 0; i < strs.size(); ++i) {
            _slots.insert_or_assign(std::make_pair(kKey, std::move(strs[i])), true);
        }
        return *this;
    }

    PlanStageReqs& clear(const Name& str) {
        _slots.erase(str);
        return *this;
    }

    bool getIsBuildingUnionForTailableCollScan() const {
        return _isBuildingUnionForTailableCollScan;
    }

    void setIsBuildingUnionForTailableCollScan(bool b) {
        _isBuildingUnionForTailableCollScan = b;
    }

    bool getIsTailableCollScanResumeBranch() const {
        return _isTailableCollScanResumeBranch;
    }

    void setIsTailableCollScanResumeBranch(bool b) {
        _isTailableCollScanResumeBranch = b;
    }

    void setTargetNamespace(const NamespaceString& nss) {
        _targetNamespace = nss;
    }

    const NamespaceString& getTargetNamespace() const {
        return _targetNamespace;
    }

    bool hasType(Type t) const {
        for (auto&& [name, isRequired] : _slots) {
            if (isRequired && name.first == t) {
                return true;
            }
        }
        return false;
    }
    bool hasFields() const {
        return hasType(kField);
    }
    bool hasKeys() const {
        return hasType(kKey);
    }

    std::vector<std::string> getOfType(Type t) const {
        std::vector<std::string> res;
        for (auto&& [name, isRequired] : _slots) {
            if (isRequired && name.first == t) {
                res.push_back(name.second);
            }
        }
        std::sort(res.begin(), res.end());
        return res;
    }
    std::vector<std::string> getFields() const {
        return getOfType(kField);
    }
    std::vector<std::string> getKeys() const {
        return getOfType(kKey);
    }

    PlanStageReqs& clearAllOfType(Type t) {
        auto fields = getOfType(t);
        for (const auto& field : fields) {
            _slots.erase(std::make_pair(t, StringData(field)));
        }
        return *this;
    }
    PlanStageReqs& clearAllFields() {
        return clearAllOfType(kField);
    }
    PlanStageReqs& clearAllKeys() {
        return clearAllOfType(kKey);
    }

    friend PlanStageSlots::PlanStageSlots(const PlanStageReqs& reqs,
                                          sbe::value::SlotIdGenerator* slotIdGenerator);

    friend void PlanStageSlots::forEachSlot(
        const PlanStageReqs& reqs, const std::function<void(sbe::value::SlotId)>& fn) const;

    friend void PlanStageSlots::forEachSlot(
        const PlanStageReqs& reqs,
        const std::function<void(sbe::value::SlotId, const Name&)>& fn) const;

    friend void PlanStageSlots::clearNonRequiredSlots(const PlanStageReqs& reqs);

private:
    PairMap<Type, std::string, bool> _slots;

    // When we're in the middle of building a special union sub-tree implementing a tailable cursor
    // collection scan, this flag will be set to true. Otherwise this flag will be false.
    bool _isBuildingUnionForTailableCollScan{false};

    // When we're in the middle of building a special union sub-tree implementing a tailable cursor
    // collection scan, this flag indicates whether we're currently building an anchor or resume
    // branch. At all other times, this flag will be false.
    bool _isTailableCollScanResumeBranch{false};

    // Tracks the current namespace that we're building a plan over. Given that the stage builder
    // can build plans for multiple namespaces, a node in the tree that targets a namespace
    // different from its parent node can set this value to notify any child nodes of the correct
    // namespace.
    NamespaceString _targetNamespace;
};

void PlanStageSlots::forEachSlot(const PlanStageReqs& reqs,
                                 const std::function<void(sbe::value::SlotId)>& fn) const {
    for (auto&& [name, isRequired] : reqs._slots) {
        if (isRequired) {
            // Clang raises an error if we attempt to use 'name' in the tassert() below, because
            // tassert() is a macro that uses lambdas and 'name' is defined via "local binding".
            // We work-around this by copying 'name' to a local variable 'slotName'.
            auto slotName = Name(name);
            auto it = _slots.find(slotName);
            tassert(7050900,
                    str::stream() << "Could not find " << static_cast<int>(slotName.first) << ":'"
                                  << slotName.second << "' in the slot map, expected slot to exist",
                    it != _slots.end());

            fn(it->second);
        }
    }
}

void PlanStageSlots::forEachSlot(
    const PlanStageReqs& reqs,
    const std::function<void(sbe::value::SlotId, const Name&)>& fn) const {
    for (auto&& [name, isRequired] : reqs._slots) {
        if (isRequired) {
            // Clang raises an error if we attempt to use 'name' in the tassert() below, because
            // tassert() is a macro that uses lambdas and 'name' is defined via "local binding".
            // We work-around this by copying 'name' to a local variable 'slotName'.
            auto slotName = Name(name);
            auto it = _slots.find(slotName);
            tassert(7050901,
                    str::stream() << "Could not find " << static_cast<int>(slotName.first) << ":'"
                                  << slotName.second << "' in the slot map, expected slot to exist",
                    it != _slots.end());

            fn(it->second, slotName);
        }
    }
}

void PlanStageSlots::clearNonRequiredSlots(const PlanStageReqs& reqs) {
    auto it = _slots.begin();
    while (it != _slots.end()) {
        auto& name = it->first;
        auto reqIt = reqs._slots.find(name);
        // We never clear kResult, regardless of whether it is required by 'reqs'.
        if ((reqIt != reqs._slots.end() && reqIt->second) ||
            (name.first == kResult.first && name.second == kResult.second)) {
            ++it;
        } else {
            _slots.erase(it++);
        }
    }
}

using InputParamToSlotMap = stdx::unordered_map<MatchExpression::InputParamId, sbe::value::SlotId>;
using VariableIdToSlotMap = stdx::unordered_map<Variables::Id, sbe::value::SlotId>;

/**
 * IndexBoundsEvaluationInfo struct contains Interval Evaluation Trees (IETs) and additional data
 * structures required to restore index bounds from IETs and bind them to generic index scan
 * algorithm.
 */
struct IndexBoundsEvaluationInfo {
    IndexEntry index;
    KeyString::Version keyStringVersion;
    Ordering ordering;
    int direction;
    std::vector<interval_evaluation_tree::IET> iets;
    ParameterizedIndexScanSlots slots;
};

/**
 * Some auxiliary data returned by a 'SlotBasedStageBuilder' along with a PlanStage tree root, which
 * is needed to execute the PlanStage tree.
 */
struct PlanStageData {
    PlanStageData(PlanStageData&&) = default;
    PlanStageData& operator=(PlanStageData&&) = default;

    explicit PlanStageData(std::unique_ptr<sbe::RuntimeEnvironment> env)
        : env(env.get()), ctx(std::move(env)) {}

    PlanStageData(const PlanStageData& other) : PlanStageData(other.env->makeDeepCopy()) {
        copyFrom(other);
    }

    PlanStageData& operator=(const PlanStageData& other) {
        if (this != &other) {
            auto envCopy = other.env->makeDeepCopy();
            env = envCopy.get();
            ctx = sbe::CompileCtx(std::move(envCopy));
            copyFrom(other);
        }
        return *this;
    }

    std::string debugString() const;

    // This holds the output slots produced by SBE plan (resultSlot, recordIdSlot, etc).
    PlanStageSlots outputs;

    // Map from index name to IAM.
    StringMap<const IndexAccessMethod*> iamMap;

    // The CompileCtx object owns the RuntimeEnvironment. The RuntimeEnvironment owns various
    // SlotAccessors which are accessed when the SBE plan is executed.
    sbe::RuntimeEnvironment* env{nullptr};
    sbe::CompileCtx ctx;

    bool shouldTrackLatestOplogTimestamp{false};
    bool shouldTrackResumeToken{false};
    bool shouldUseTailableScan{false};

    // If this execution tree was built as a result of replanning of the cached plan, this string
    // will include the reason for replanning.
    boost::optional<std::string> replanReason;

    // If this candidate plan has completed the trial run early by achieving one of the trial run
    // metrics, the stats are cached in here.
    std::unique_ptr<sbe::PlanStageStats> savedStatsOnEarlyExit{nullptr};

    // Stores plan cache entry information used as debug information or for "explain" purpose.
    // Note that 'debugInfo' is present only if this PlanStageData is recovered from the plan cache.
    std::shared_ptr<const plan_cache_debug_info::DebugInfoSBE> debugInfo;

    // If the query has been auto-parameterized, then the mapping from input parameter id to the
    // id of a slot in the runtime environment is maintained here. This mapping is established
    // during stage building and stored in the cache. When a cached plan is used for a subsequent
    // query, this mapping is used to set the new constant value associated with each input
    // parameter id in the runtime environment.
    //
    // For example, imagine an auto-parameterized query {a: <p1>, b: <p2>} is present in the SBE
    // plan cache. Also present in the cache is this mapping:
    //    p1 -> s3
    //    p2 -> s4
    //
    // A new query {a: 5, b: 6} runs. Using this mapping, we set a value of 5 in s3 and 6 in s4.
    InputParamToSlotMap inputParamToSlotMap;
    // This Variable-to-SlotId map stores all the Variables that were translated into corresponding
    // SBE Slots. The slots are registered in the 'RuntimeEnvironment'.
    VariableIdToSlotMap variableIdToSlotMap;

    // Stores auxiliary data to restore index bounds for a cached auto-parameterized SBE plan for
    // every index used by the plan.
    std::vector<IndexBoundsEvaluationInfo> indexBoundsEvaluationInfos;

    // Stores all namespaces involved in the build side of a hash join plan. Needed to check if the
    // plan should be evicted as the size of the foreign namespace changes.
    stdx::unordered_set<NamespaceString> foreignHashJoinCollections;

private:
    // This copy function copies data from 'other' but will not create a copy of its
    // RuntimeEnvironment and CompileCtx.
    void copyFrom(const PlanStageData& other) {
        outputs = other.outputs;
        iamMap = other.iamMap;
        shouldTrackLatestOplogTimestamp = other.shouldTrackLatestOplogTimestamp;
        shouldTrackResumeToken = other.shouldTrackResumeToken;
        shouldUseTailableScan = other.shouldUseTailableScan;
        replanReason = other.replanReason;
        if (other.savedStatsOnEarlyExit) {
            savedStatsOnEarlyExit.reset(other.savedStatsOnEarlyExit->clone());
        } else {
            savedStatsOnEarlyExit.reset();
        }
        if (other.debugInfo) {
            debugInfo = std::make_unique<plan_cache_debug_info::DebugInfoSBE>(*other.debugInfo);
        } else {
            debugInfo.reset();
        }
        inputParamToSlotMap = other.inputParamToSlotMap;
        variableIdToSlotMap = other.variableIdToSlotMap;
        indexBoundsEvaluationInfos = other.indexBoundsEvaluationInfos;
        foreignHashJoinCollections = other.foreignHashJoinCollections;
    }
};

/**
 * A stage builder which builds an executable tree using slot-based PlanStages.
 */
class SlotBasedStageBuilder final : public StageBuilder<sbe::PlanStage> {
public:
    static constexpr auto kResult = PlanStageSlots::kResult;
    static constexpr auto kRecordId = PlanStageSlots::kRecordId;
    static constexpr auto kReturnKey = PlanStageSlots::kReturnKey;
    static constexpr auto kSnapshotId = PlanStageSlots::kSnapshotId;
    static constexpr auto kIndexId = PlanStageSlots::kIndexId;
    static constexpr auto kIndexKey = PlanStageSlots::kIndexKey;
    static constexpr auto kIndexKeyPattern = PlanStageSlots::kIndexKeyPattern;

    SlotBasedStageBuilder(OperationContext* opCtx,
                          const MultipleCollectionAccessor& collections,
                          const CanonicalQuery& cq,
                          const QuerySolution& solution,
                          PlanYieldPolicySBE* yieldPolicy);

    /**
     * This method will build an SBE PlanStage tree for QuerySolutionNode 'root' and its
     * descendents.
     *
     * This method is a wrapper around 'build(const QuerySolutionNode*, const PlanStageReqs&)'.
     */
    std::unique_ptr<sbe::PlanStage> build(const QuerySolutionNode* root) final;

    PlanStageData getPlanStageData() {
        return std::move(_data);
    }

private:
    /**
     * This method will build an SBE PlanStage tree for QuerySolutionNode 'root' and its
     * descendents.
     *
     * Based on the type of 'root', this method will dispatch to the appropriate buildXXX() method.
     * This method will also handle generating calls to getField() to satisfy kField reqs that were
     * not satisfied by the buildXXX() method.
     */
    std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> build(const QuerySolutionNode* node,
                                                                     const PlanStageReqs& reqs);

    std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> buildCollScan(
        const QuerySolutionNode* root, const PlanStageReqs& reqs);

    std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> buildVirtualScan(
        const QuerySolutionNode* root, const PlanStageReqs& reqs);

    std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> buildIndexScan(
        const QuerySolutionNode* root, const PlanStageReqs& reqs);

    std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> buildColumnScan(
        const QuerySolutionNode* root, const PlanStageReqs& reqs);

    std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> buildFetch(
        const QuerySolutionNode* root, const PlanStageReqs& reqs);

    std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> buildLimit(
        const QuerySolutionNode* root, const PlanStageReqs& reqs);

    std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> buildSkip(
        const QuerySolutionNode* root, const PlanStageReqs& reqs);

    std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> buildSort(
        const QuerySolutionNode* root, const PlanStageReqs& reqs);

    std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> buildSortCovered(
        const QuerySolutionNode* root, const PlanStageReqs& reqs);

    std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> buildSortKeyGenerator(
        const QuerySolutionNode* root, const PlanStageReqs& reqs);

    std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> buildSortMerge(
        const QuerySolutionNode* root, const PlanStageReqs& reqs);

    std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> buildProjectionSimple(
        const QuerySolutionNode* root, const PlanStageReqs& reqs);

    std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> buildProjectionCovered(
        const QuerySolutionNode* root, const PlanStageReqs& reqs);

    std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> buildProjectionDefault(
        const QuerySolutionNode* root, const PlanStageReqs& reqs);

    std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> buildProjectionDefaultCovered(
        const QuerySolutionNode* root, const PlanStageReqs& reqs);

    std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> buildOr(
        const QuerySolutionNode* root, const PlanStageReqs& reqs);

    std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> buildTextMatch(
        const QuerySolutionNode* root, const PlanStageReqs& reqs);

    std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> buildReturnKey(
        const QuerySolutionNode* root, const PlanStageReqs& reqs);

    std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> buildEof(
        const QuerySolutionNode* root, const PlanStageReqs& reqs);

    std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> buildAndHash(
        const QuerySolutionNode* root, const PlanStageReqs& reqs);

    std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> buildAndSorted(
        const QuerySolutionNode* root, const PlanStageReqs& reqs);

    std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> makeUnionForTailableCollScan(
        const QuerySolutionNode* root, const PlanStageReqs& reqs);

    std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> buildShardFilter(
        const QuerySolutionNode* root, const PlanStageReqs& reqs);

    /**
     * Constructs an optimized SBE plan for 'root' in the case that the fields of the shard key
     * pattern are provided by the child index scan. In this case, the SBE plan for the child
     * index scan node will fill out slots for the necessary components of the index key. These
     * slots can be read directly in order to determine the shard key that should be passed to the
     * 'shardFiltererSlot'.
     */
    std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> buildShardFilterCovered(
        const QuerySolutionNode* root, const PlanStageReqs& reqs);

    std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> buildGroup(
        const QuerySolutionNode* root, const PlanStageReqs& reqs);

    std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> buildLookup(
        const QuerySolutionNode* root, const PlanStageReqs& reqs);

    /**
     * Returns a CollectionPtr corresponding to the collection that we are currently building a
     * plan over. If no current namespace is configured, a CollectionPtr referencing the main
     * collection tracked by '_collections' is returned.
     */
    const CollectionPtr& getCurrentCollection(const PlanStageReqs& reqs) const;

    sbe::value::SlotIdGenerator _slotIdGenerator;
    sbe::value::FrameIdGenerator _frameIdGenerator;
    sbe::value::SpoolIdGenerator _spoolIdGenerator;

    const MultipleCollectionAccessor& _collections;

    // Indicates the main namespace that we're building a plan over.
    NamespaceString _mainNss;

    PlanYieldPolicySBE* const _yieldPolicy{nullptr};

    // Apart from generating just an execution tree, this builder will also produce some auxiliary
    // data which is needed to execute the tree.
    PlanStageData _data;

    bool _buildHasStarted{false};

    // Common parameters to SBE stage builder functions.
    StageBuilderState _state;
};
}  // namespace mongo::stage_builder
