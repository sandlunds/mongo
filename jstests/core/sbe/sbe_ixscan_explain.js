// Tests if explain output for index scans and index seeks stages contains the indexName in the
// executionStats output.
//
// @tags: [
//   assumes_against_mongod_not_mongos,
//   requires_fcv_51,
// ]

(function() {
"use strict";

load('jstests/libs/analyze_plan.js');  // For getPlanStages
load("jstests/libs/sbe_util.js");      // For checkSBEEnabled.

const isSBEEnabled = checkSBEEnabled(db, ["featureFlagSbeFull"], true /* checkAllNodes */);
if (!isSBEEnabled) {
    jsTestLog("Skipping test because the SBE feature flag is disabled");
    return;
}

function assertStageContainsIndexName(stage) {
    assert(stage.hasOwnProperty("indexName"));
    assert.eq(stage["indexName"], "a_1", stage);
}

const coll = db.sbe_ixscan_explain;
coll.drop();
assert.commandWorked(coll.createIndex({a: 1}));

assert.commandWorked(coll.insertMany([
    {_id: 0, a: 1, b: 1, c: 1},
    {_id: 1, a: 2, b: 1, c: 2},
    {_id: 2, a: 3, b: 1, c: 3},
    {_id: 3, a: 4, b: 2, c: 4}
]));

let explain = coll.find({a: 3}).hint({a: 1}).explain("executionStats");
assert(isIxscan(db, getWinningPlan(explain.queryPlanner)));
// Ensure the query is run on sbe engine.
assert('slotBasedPlan' in explain.queryPlanner.winningPlan);

let ixscanStages = getPlanStages(explain.executionStats.executionStages, "ixseek");
assert(ixscanStages.length !== 0);
for (let ixscanStage of ixscanStages) {
    assertStageContainsIndexName(ixscanStage);
}
}());
