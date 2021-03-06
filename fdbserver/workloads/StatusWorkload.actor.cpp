/*
 * StatusWorkload.actor.cpp
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2018 Apple Inc. and the FoundationDB project authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "fdbclient/NativeAPI.h"
#include "fdbserver/TesterInterface.h"
#include "fdbserver/workloads/workloads.h"
#include "fdbclient/StatusClient.h"
#include "flow/UnitTest.h"
#include "fdbclient/Schemas.h"
#include "fdbclient/ManagementAPI.h"
#include "flow/actorcompiler.h"  // This must be the last #include.

extern bool noUnseed;

struct StatusWorkload : TestWorkload {
	double testDuration, requestsPerSecond;

	PerfIntCounter requests, replies, errors, totalSize;
	Optional<StatusObject> parsedSchema;

	StatusWorkload(WorkloadContext const& wcx)
		: TestWorkload(wcx),
		requests("Status requests issued"), replies("Status replies received"), errors("Status Errors"), totalSize("Status reply size sum")
	{
		testDuration = getOption(options, LiteralStringRef("testDuration"), 10.0);
		requestsPerSecond = getOption(options, LiteralStringRef("requestsPerSecond"), 0.5);
		auto statusSchemaStr = getOption(options, LiteralStringRef("schema"), JSONSchemas::statusSchema);
		if (statusSchemaStr.size()) {
			json_spirit::mValue schema = readJSONStrictly(statusSchemaStr.toString());
			parsedSchema = schema.get_obj();

			// This is sort of a hack, but generate code coverage *requirements* for everything in schema
			schemaCoverageRequirements(parsedSchema.get());
		}

		noUnseed = true;
	}

	virtual std::string description() { return "StatusWorkload"; }
	virtual Future<Void> setup(Database const& cx) {
		return Void();
	}
	virtual Future<Void> start(Database const& cx) {
		if (clientId != 0)
			return Void();
		Reference<Cluster> cluster = cx->cluster;
		if (!cluster) {
			TraceEvent(SevError, "StatusWorkloadStartError").detail("Reason", "NULL cluster");
			return Void();
		}
		return success(timeout(fetcher(cluster->getConnectionFile(), this), testDuration));
	}
	virtual Future<bool> check(Database const& cx) {
		return errors.getValue() == 0;
	}

	virtual void getMetrics(vector<PerfMetric>& m) {
		if (clientId != 0)
			return;

		m.push_back(requests.getMetric());
		m.push_back(replies.getMetric());
		m.push_back(PerfMetric("Average Reply Size", replies.getValue() ? totalSize.getValue() / replies.getValue() : 0, false));
		m.push_back(errors.getMetric());
	}

	static void schemaCoverageRequirements( StatusObject const& schema, std::string schema_path = std::string() ) {
		try {
			for(auto& skv : schema) {
				std::string spath = schema_path + "." + skv.first;

				schemaCoverage(spath, false);

				if (skv.second.type() == json_spirit::array_type && skv.second.get_array().size()) {
					schemaCoverageRequirements( skv.second.get_array()[0].get_obj(), spath + "[0]" );
				} else if (skv.second.type() == json_spirit::obj_type) {
					if (skv.second.get_obj().count("$enum")) {
						for(auto& enum_item : skv.second.get_obj().at("$enum").get_array())
							schemaCoverage(spath + ".$enum." + enum_item.get_str(), false);
					} else
						schemaCoverageRequirements( skv.second.get_obj(), spath );
				}
			}
		} catch (std::exception& e) {
			TraceEvent(SevError,"SchemaCoverageRequirementsException").detail("What", e.what());
			throw unknown_error();
		} catch (...) {
			TraceEvent(SevError,"SchemaCoverageRequirementsException");
			throw unknown_error();
		}
	}

	ACTOR Future<Void> fetcher(Reference<ClusterConnectionFile> connFile, StatusWorkload *self) {
		state double lastTime = now();

		loop{
			wait(poisson(&lastTime, 1.0 / self->requestsPerSecond));
			try {
				// Since we count the requests that start, we could potentially never really hear back?
				++self->requests;
				state double issued = now();
				StatusObject result = wait(StatusClient::statusFetcher(connFile));
				++self->replies;
				BinaryWriter br(AssumeVersion(currentProtocolVersion));
				save(br, result);
				self->totalSize += br.getLength();
				TraceEvent("StatusWorkloadReply").detail("ReplySize", br.getLength()).detail("Latency", now() - issued);//.detail("Reply", json_spirit::write_string(json_spirit::mValue(result)));
				std::string errorStr;
				if (self->parsedSchema.present() && !schemaMatch(self->parsedSchema.get(), result, errorStr, SevError, true) )
					TraceEvent(SevError, "StatusWorkloadValidationFailed").detail("JSON", json_spirit::write_string(json_spirit::mValue(result)));
			}
			catch (Error& e) {
				if (e.code() != error_code_actor_cancelled) {
					TraceEvent(SevError, "StatusWorkloadError").error(e);
					++self->errors;
				}
				throw;
			}
		}
	}

};

WorkloadFactory<StatusWorkload> StatusWorkloadFactory("Status");

TEST_CASE("/fdbserver/status/schema/basic") {
	json_spirit::mValue schema = readJSONStrictly("{\"apple\":3,\"banana\":\"foo\",\"sub\":{\"thing\":true},\"arr\":[{\"a\":1,\"b\":2}],\"en\":{\"$enum\":[\"foo\",\"bar\"]},\"mapped\":{\"$map\":{\"x\":true}}}");
	auto check = [&schema](bool expect_ok, std::string t) {
		json_spirit::mValue test = readJSONStrictly(t);
		TraceEvent("SchemaMatch").detail("Schema", json_spirit::write_string(schema)).detail("Value", json_spirit::write_string(test)).detail("Expect", expect_ok);
		std::string errorStr;
		ASSERT( expect_ok == schemaMatch(schema.get_obj(), test.get_obj(), errorStr, expect_ok ? SevError : SevInfo, true) );
	};
	check(true, "{}");
	check(true, "{\"apple\":4}");
	check(false, "{\"apple\":\"wrongtype\"}");
	check(false, "{\"extrathingy\":1}");
	check(true, "{\"banana\":\"b\",\"sub\":{\"thing\":false}}");
	check(false, "{\"banana\":\"b\",\"sub\":{\"thing\":false, \"x\":0}}");
	check(true, "{\"arr\":[{},{\"a\":0}]}");
	check(false, "{\"arr\":[{\"a\":0},{\"c\":0}]}");
	check(true, "{\"en\":\"bar\"}");
	check(false, "{\"en\":\"baz\"}");
	check(true, "{\"mapped\":{\"item1\":{\"x\":false},\"item2\":{}}}");
	check(false, "{\"mapped\":{\"item1\":{\"x\":false},\"item2\":{\"y\":1}}}");

	return Void();
}
