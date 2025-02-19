////////////////////////////////////////////////////////////////////////////
//
// Copyright 2016 Realm Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
////////////////////////////////////////////////////////////////////////////

#include "collection_fixtures.hpp"
#include "util/sync/baas_admin_api.hpp"
#include "util/sync/sync_test_utils.hpp"
#include "util/test_path.hpp"
#include "util/unit_test_transport.hpp"

#include <realm/object-store/impl/object_accessor_impl.hpp>
#include <realm/object-store/sync/app_credentials.hpp>
#include <realm/object-store/sync/app_utils.hpp>
#include <realm/object-store/sync/async_open_task.hpp>
#include <realm/object-store/sync/generic_network_transport.hpp>
#include <realm/object-store/sync/mongo_client.hpp>
#include <realm/object-store/sync/mongo_collection.hpp>
#include <realm/object-store/sync/mongo_database.hpp>
#include <realm/object-store/sync/sync_session.hpp>
#include <realm/object-store/sync/sync_user.hpp>
#include <realm/object-store/thread_safe_reference.hpp>
#include <realm/object-store/util/uuid.hpp>
#include <realm/sync/network/default_socket.hpp>
#include <realm/sync/network/websocket.hpp>
#include <realm/sync/noinst/server/access_token.hpp>
#include <realm/util/base64.hpp>
#include <realm/util/overload.hpp>
#include <realm/util/platform_info.hpp>
#include <realm/util/future.hpp>
#include <realm/util/uri.hpp>

#include <catch2/catch_all.hpp>
#include <external/json/json.hpp>
#include <external/mpark/variant.hpp>

#include <condition_variable>
#include <iostream>
#include <list>
#include <mutex>

using namespace realm;
using namespace realm::app;
using util::any_cast;
using util::Optional;

using namespace std::string_view_literals;
using namespace std::literals::string_literals;

namespace {
std::shared_ptr<SyncUser> log_in(std::shared_ptr<App> app, AppCredentials credentials = AppCredentials::anonymous())
{
    if (auto transport = dynamic_cast<UnitTestTransport*>(app->config().transport.get())) {
        transport->set_provider_type(credentials.provider_as_string());
    }
    std::shared_ptr<SyncUser> user;
    app->log_in_with_credentials(credentials, [&](std::shared_ptr<SyncUser> user_arg, Optional<AppError> error) {
        REQUIRE_FALSE(error);
        REQUIRE(user_arg);
        user = std::move(user_arg);
    });
    REQUIRE(user);
    return user;
}

AppError failed_log_in(std::shared_ptr<App> app, AppCredentials credentials = AppCredentials::anonymous())
{
    Optional<AppError> err;
    app->log_in_with_credentials(credentials, [&](std::shared_ptr<SyncUser> user, Optional<AppError> error) {
        REQUIRE(error);
        REQUIRE_FALSE(user);
        err = error;
    });
    REQUIRE(err);
    return *err;
}

} // namespace

namespace realm {
class TestHelper {
public:
    static DBRef get_db(Realm& realm)
    {
        return Realm::Internal::get_db(realm);
    }
};
} // namespace realm

static const std::string profile_0_name = "Ursus americanus Ursus boeckhi";
static const std::string profile_0_first_name = "Ursus americanus";
static const std::string profile_0_last_name = "Ursus boeckhi";
static const std::string profile_0_email = "Ursus ursinus";
static const std::string profile_0_picture_url = "Ursus malayanus";
static const std::string profile_0_gender = "Ursus thibetanus";
static const std::string profile_0_birthday = "Ursus americanus";
static const std::string profile_0_min_age = "Ursus maritimus";
static const std::string profile_0_max_age = "Ursus arctos";

static const nlohmann::json profile_0 = {
    {"name", profile_0_name},         {"first_name", profile_0_first_name},   {"last_name", profile_0_last_name},
    {"email", profile_0_email},       {"picture_url", profile_0_picture_url}, {"gender", profile_0_gender},
    {"birthday", profile_0_birthday}, {"min_age", profile_0_min_age},         {"max_age", profile_0_max_age}};

static nlohmann::json user_json(std::string access_token, std::string user_id = random_string(15))
{
    return {{"access_token", access_token},
            {"refresh_token", access_token},
            {"user_id", user_id},
            {"device_id", "Panda Bear"}};
}

static nlohmann::json user_profile_json(std::string user_id = random_string(15),
                                        std::string identity_0_id = "Ursus arctos isabellinus",
                                        std::string identity_1_id = "Ursus arctos horribilis",
                                        std::string provider_type = "anon-user")
{
    return {{"user_id", user_id},
            {"identities",
             {{{"id", identity_0_id}, {"provider_type", provider_type}},
              {{"id", identity_1_id}, {"provider_type", "lol_wut"}}}},
            {"data", profile_0}};
}

static const std::string good_access_token =
    "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9."
    "eyJleHAiOjE1ODE1MDc3OTYsImlhdCI6MTU4MTUwNTk5NiwiaXNzIjoiNWU0M2RkY2M2MzZlZTEwNmVhYTEyYmRjIiwic3RpdGNoX2RldklkIjoi"
    "MDAwMDAwMDAwMDAwMDAwMDAwMDAwMDAwIiwic3RpdGNoX2RvbWFpbklkIjoiNWUxNDk5MTNjOTBiNGFmMGViZTkzNTI3Iiwic3ViIjoiNWU0M2Rk"
    "Y2M2MzZlZTEwNmVhYTEyYmRhIiwidHlwIjoiYWNjZXNzIn0.0q3y9KpFxEnbmRwahvjWU1v9y1T1s3r2eozu93vMc3s";

static const std::string good_access_token2 =
    "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9."
    "eyJleHAiOjE1ODkzMDE3MjAsImlhdCI6MTU4NDExODcyMCwiaXNzIjoiNWU2YmJiYzBhNmI3ZGZkM2UyNTA0OGI3Iiwic3RpdGNoX2RldklkIjoi"
    "MDAwMDAwMDAwMDAwMDAwMDAwMDAwMDAwIiwic3RpdGNoX2RvbWFpbklkIjoiNWUxNDk5MTNjOTBiNGFmMGViZTkzNTI3Iiwic3ViIjoiNWU2YmJi"
    "YzBhNmI3ZGZkM2UyNTA0OGIzIiwidHlwIjoiYWNjZXNzIn0.eSX4QMjIOLbdOYOPzQrD_racwLUk1HGFgxtx2a34k80";

#if REALM_ENABLE_AUTH_TESTS

#include <realm/util/sha_crypto.hpp>

static std::string create_jwt(const std::string& appId)
{
    nlohmann::json header = {{"alg", "HS256"}, {"typ", "JWT"}};
    nlohmann::json payload = {{"aud", appId}, {"sub", "someUserId"}, {"exp", 1961896476}};

    payload["user_data"]["name"] = "Foo Bar";
    payload["user_data"]["occupation"] = "firefighter";

    payload["my_metadata"]["name"] = "Bar Foo";
    payload["my_metadata"]["occupation"] = "stock analyst";

    std::string header_str = header.dump();
    std::string payload_str = payload.dump();

    std::string encoded_header;
    encoded_header.resize(util::base64_encoded_size(header_str.length()));
    util::base64_encode(header_str, encoded_header);

    std::string encoded_payload;
    encoded_payload.resize(util::base64_encoded_size(payload_str.length()));
    util::base64_encode(payload_str, encoded_payload);

    // Remove padding characters.
    while (encoded_header.back() == '=')
        encoded_header.pop_back();
    while (encoded_payload.back() == '=')
        encoded_payload.pop_back();

    std::string jwtPayload = encoded_header + "." + encoded_payload;

    std::array<char, 32> hmac;
    unsigned char key[] = "My_very_confidential_secretttttt";
    util::hmac_sha256(util::unsafe_span_cast<uint8_t>(jwtPayload), util::unsafe_span_cast<uint8_t>(hmac),
                      util::Span<uint8_t, 32>(key, 32));

    std::string signature;
    signature.resize(util::base64_encoded_size(hmac.size()));
    util::base64_encode(hmac, signature);
    while (signature.back() == '=')
        signature.pop_back();
    std::replace(signature.begin(), signature.end(), '+', '-');
    std::replace(signature.begin(), signature.end(), '/', '_');

    return jwtPayload + "." + signature;
}

// MARK: - Verify AppError with all error codes
TEST_CASE("app: verify app error codes", "[sync][app][local]") {
    auto error_codes = ErrorCodes::get_error_list();
    std::vector<std::pair<int, std::string>> http_status_codes = {
        {0, ""},
        {100, "http error code considered fatal: some http error. Informational: 100"},
        {200, ""},
        {300, "http error code considered fatal: some http error. Redirection: 300"},
        {400, "http error code considered fatal: some http error. Client Error: 400"},
        {500, "http error code considered fatal: some http error. Server Error: 500"},
        {600, "http error code considered fatal: some http error. Unknown HTTP Error: 600"}};

    auto make_http_error = [](std::optional<std::string_view> error_code, int http_status = 500,
                              std::optional<std::string_view> error = "some error",
                              std::optional<std::string_view> link = "http://dummy-link/") -> app::Response {
        nlohmann::json body;
        if (error_code) {
            body["error_code"] = *error_code;
        }
        if (error) {
            body["error"] = *error;
        }
        if (link) {
            body["link"] = *link;
        }

        return {
            http_status,
            0,
            {{"Content-Type", "application/json"}},
            body.empty() ? "{}" : body.dump(),
        };
    };

    auto validate_json_body = [](std::string body, std::optional<std::string_view> error_code,
                                 std::optional<std::string_view> error = "some error",
                                 std::optional<std::string_view> logs_link = "http://dummy-link/") -> bool {
        if (body.empty()) {
            return false;
        }
        try {
            auto json_body = nlohmann::json::parse(body);
            // If provided, check the error_code value against the 'error_code' value in the json body
            auto code = json_body.find("error_code");
            if (error_code && !error_code->empty()) {
                if (code == json_body.end() || code->get<std::string>() != *error_code) {
                    return false;
                }
            }
            // If not provided, it's an error if the value is included in the json body
            else if (code != json_body.end()) {
                return false;
            }
            // If provided, check the message value against the 'error' value in the json body
            auto message = json_body.find("error");
            if (error && !error->empty()) {
                if (message == json_body.end() || message->get<std::string>() != *error) {
                    return false;
                }
            }
            // If not provided, it's an error if the value is included in the json body
            else if (message != json_body.end()) {
                return false;
            }
            // If provided, check the logs_link value against the 'link' value in the json body
            auto link = json_body.find("link");
            if (logs_link && !logs_link->empty()) {
                if (link == json_body.end() || link->get<std::string>() != *logs_link) {
                    return false;
                }
            }
            // If not provided, it's an error if the value is included in the json body
            else if (link != json_body.end()) {
                return false;
            }
        }
        catch (const nlohmann::json::exception& ex) {
            // It's also a failure if parsing the json body throws an exception
            return false;
        }
        return true;
    };

    // Success responses
    app::Response response = {200, 0, {}, ""};
    auto app_error = AppUtils::check_for_errors(response);
    REQUIRE(!app_error);

    response = {0, 0, {}, ""};
    app_error = AppUtils::check_for_errors(response);
    REQUIRE(!app_error);

    // Empty error code
    response = make_http_error("");
    app_error = AppUtils::check_for_errors(response);
    REQUIRE(app_error);
    REQUIRE(app_error->code() == ErrorCodes::AppUnknownError);
    REQUIRE(app_error->code_string() == "AppUnknownError");
    REQUIRE(app_error->server_error.empty());
    REQUIRE(app_error->reason() == "some error");
    REQUIRE(app_error->link_to_server_logs == "http://dummy-link/");
    REQUIRE(*app_error->additional_status_code == 500);

    // Re-compose back into a Response
    auto err_response = AppUtils::make_apperror_response(*app_error);
    REQUIRE(err_response.http_status_code == 500);
    REQUIRE(!err_response.body.empty());
    REQUIRE(validate_json_body(err_response.body, ""));
    REQUIRE(!err_response.client_error_code);
    REQUIRE(err_response.custom_status_code == 0);
    auto ct = AppUtils::find_header("content-type", err_response.headers);
    REQUIRE(ct);
    REQUIRE(ct->second == "application/json");

    // Missing error code
    response = make_http_error(std::nullopt);
    app_error = AppUtils::check_for_errors(response);
    REQUIRE(app_error);
    REQUIRE(app_error->code() == ErrorCodes::AppUnknownError);
    REQUIRE(app_error->code_string() == "AppUnknownError");
    REQUIRE(app_error->server_error.empty());
    REQUIRE(app_error->reason() == "some error");
    REQUIRE(app_error->link_to_server_logs == "http://dummy-link/");
    REQUIRE(*app_error->additional_status_code == 500);

    // Re-compose back into a Response
    err_response = AppUtils::make_apperror_response(*app_error);
    REQUIRE(err_response.http_status_code == 500);
    REQUIRE(!err_response.body.empty());
    REQUIRE(validate_json_body(err_response.body, std::nullopt));
    REQUIRE(!err_response.client_error_code);
    REQUIRE(err_response.custom_status_code == 0);
    ct = AppUtils::find_header("content-type", err_response.headers);
    REQUIRE(ct);
    REQUIRE(ct->second == "application/json");

    // Missing error message
    response = make_http_error("InvalidParameter", 404, std::nullopt);
    app_error = AppUtils::check_for_errors(response);
    REQUIRE(app_error);
    REQUIRE(app_error->code() == ErrorCodes::InvalidParameter);
    REQUIRE(app_error->code_string() == "InvalidParameter");
    REQUIRE(app_error->server_error == "InvalidParameter");
    REQUIRE(app_error->reason() == "no error message");
    REQUIRE(app_error->link_to_server_logs == "http://dummy-link/");
    REQUIRE(*app_error->additional_status_code == 404);

    // Re-compose back into a Response
    err_response = AppUtils::make_apperror_response(*app_error);
    REQUIRE(err_response.http_status_code == 404);
    REQUIRE(!err_response.body.empty());
    REQUIRE(validate_json_body(err_response.body, "InvalidParameter", "no error message"));
    REQUIRE(!err_response.client_error_code);
    REQUIRE(err_response.custom_status_code == 0);
    ct = AppUtils::find_header("content-type", err_response.headers);
    REQUIRE(ct);
    REQUIRE(ct->second == "application/json");

    // Missing logs link
    response = make_http_error("InvalidParameter", 403, "some error occurred", std::nullopt);
    app_error = AppUtils::check_for_errors(response);
    REQUIRE(app_error);
    REQUIRE(app_error->code() == ErrorCodes::InvalidParameter);
    REQUIRE(app_error->code_string() == "InvalidParameter");
    REQUIRE(app_error->server_error == "InvalidParameter");
    REQUIRE(app_error->reason() == "some error occurred");
    REQUIRE(app_error->link_to_server_logs == "");
    REQUIRE(*app_error->additional_status_code == 403);

    // Re-compose back into a Response
    err_response = AppUtils::make_apperror_response(*app_error);
    REQUIRE(err_response.http_status_code == 403);
    REQUIRE(!err_response.body.empty());
    REQUIRE(validate_json_body(err_response.body, "InvalidParameter", "some error occurred", std::nullopt));
    REQUIRE(!err_response.client_error_code);
    REQUIRE(err_response.custom_status_code == 0);
    ct = AppUtils::find_header("content-type", err_response.headers);
    REQUIRE(ct);
    REQUIRE(ct->second == "application/json");

    // Missing error code and error message with success http status
    response = make_http_error(std::nullopt, 200, std::nullopt);
    app_error = AppUtils::check_for_errors(response);
    REQUIRE(!app_error);

    for (auto [name, error] : error_codes) {
        // All error codes should not cause an exception
        if (error != ErrorCodes::HTTPError && error != ErrorCodes::OK) {
            response = make_http_error(name);
            app_error = AppUtils::check_for_errors(response);
            REQUIRE(app_error);
            if (ErrorCodes::error_categories(error).test(ErrorCategory::app_error)) {
                REQUIRE(app_error->code() == error);
                REQUIRE(app_error->code_string() == name);
            }
            else {
                REQUIRE(app_error->code() == ErrorCodes::AppServerError);
                REQUIRE(app_error->code_string() == "AppServerError");
            }
            REQUIRE(app_error->server_error == name);
            REQUIRE(app_error->reason() == "some error");
            REQUIRE(app_error->link_to_server_logs == "http://dummy-link/");
            REQUIRE(app_error->additional_status_code);
            REQUIRE(*app_error->additional_status_code == 500);

            // Re-compose back into a Response
            err_response = AppUtils::make_apperror_response(*app_error);
            REQUIRE(err_response.http_status_code == 500);
            REQUIRE(!err_response.body.empty());
            REQUIRE(validate_json_body(err_response.body, name));
            REQUIRE(!err_response.client_error_code);
            REQUIRE(err_response.custom_status_code == 0);
            ct = AppUtils::find_header("content-type", err_response.headers);
            REQUIRE(ct);
            REQUIRE(ct->second == "application/json");
        }
    }

    response = make_http_error("AppErrorMissing", 404);
    app_error = AppUtils::check_for_errors(response);
    REQUIRE(app_error);
    REQUIRE(app_error->code() == ErrorCodes::AppServerError);
    REQUIRE(app_error->code_string() == "AppServerError");
    REQUIRE(app_error->server_error == "AppErrorMissing");
    REQUIRE(app_error->reason() == "some error");
    REQUIRE(app_error->link_to_server_logs == "http://dummy-link/");
    REQUIRE(app_error->additional_status_code);
    REQUIRE(*app_error->additional_status_code == 404);

    // Re-compose back into a Response
    err_response = AppUtils::make_apperror_response(*app_error);
    REQUIRE(err_response.http_status_code == 404);
    REQUIRE(!err_response.body.empty());
    REQUIRE(validate_json_body(err_response.body, "AppErrorMissing"));
    REQUIRE(!err_response.client_error_code);
    REQUIRE(err_response.custom_status_code == 0);
    ct = AppUtils::find_header("content-type", err_response.headers);
    REQUIRE(ct);
    REQUIRE(ct->second == "application/json");

    // HTTPError with different status values
    for (auto [status, message] : http_status_codes) {
        response = {
            status,
            0,
            {},
            "some http error",
        };
        app_error = AppUtils::check_for_errors(response);
        if (message.empty()) {
            REQUIRE(!app_error);
            continue;
        }
        REQUIRE(app_error);
        REQUIRE(app_error->code() == ErrorCodes::HTTPError);
        REQUIRE(app_error->code_string() == "HTTPError");
        REQUIRE(app_error->server_error.empty());
        REQUIRE(app_error->reason() == message);
        REQUIRE(app_error->link_to_server_logs.empty());
        REQUIRE(app_error->additional_status_code);
        REQUIRE(*app_error->additional_status_code == status);

        // Recompose back into a Response
        err_response = AppUtils::make_apperror_response(*app_error);
        REQUIRE(err_response.http_status_code == status);
        REQUIRE(err_response.body == "some http error");
        REQUIRE(!err_response.client_error_code);
        REQUIRE(err_response.custom_status_code == 0);
        REQUIRE(err_response.headers.empty());
    }

    // Missing error code and error message with fatal http status
    response = {
        501,
        0,
        {},
        "",
    };
    app_error = AppUtils::check_for_errors(response);
    REQUIRE(app_error);
    REQUIRE(app_error->code() == ErrorCodes::HTTPError);
    REQUIRE(app_error->code_string() == "HTTPError");
    REQUIRE(app_error->server_error.empty());
    REQUIRE(app_error->reason() == "http error code considered fatal. Server Error: 501");
    REQUIRE(app_error->link_to_server_logs.empty());
    REQUIRE(app_error->additional_status_code);
    REQUIRE(*app_error->additional_status_code == 501);

    // Re-compose back into a Response
    err_response = AppUtils::make_apperror_response(*app_error);
    REQUIRE(err_response.http_status_code == 501);
    REQUIRE(err_response.body.empty());
    REQUIRE(!err_response.client_error_code);
    REQUIRE(err_response.custom_status_code == 0);
    REQUIRE(err_response.headers.empty());

    // Missing error code and error message contains period with redirect http status
    response = {
        308,
        0,
        {},
        "some http error. ocurred",
    };
    app_error = AppUtils::check_for_errors(response);
    REQUIRE(app_error);
    REQUIRE(app_error->code() == ErrorCodes::HTTPError);
    REQUIRE(app_error->code_string() == "HTTPError");
    REQUIRE(app_error->server_error.empty());
    REQUIRE(app_error->reason() == "http error code considered fatal: some http error. ocurred. Redirection: 308");
    REQUIRE(app_error->link_to_server_logs.empty());
    REQUIRE(app_error->additional_status_code);
    REQUIRE(*app_error->additional_status_code == 308);

    // Re-compose back into a Response
    err_response = AppUtils::make_apperror_response(*app_error);
    REQUIRE(err_response.http_status_code == 308);
    REQUIRE(err_response.body == "some http error. ocurred");
    REQUIRE(!err_response.client_error_code);
    REQUIRE(err_response.custom_status_code == 0);
    REQUIRE(err_response.headers.empty());

    // Valid client error code, with body, but no json
    app::Response client_response = {
        501,
        0,
        {},
        "Some error occurred",
        ErrorCodes::BadBsonParse, // client_error_code
    };
    app_error = AppUtils::check_for_errors(client_response);
    REQUIRE(app_error);
    REQUIRE(app_error->code() == ErrorCodes::BadBsonParse);
    REQUIRE(app_error->code_string() == "BadBsonParse");
    REQUIRE(app_error->server_error.empty());
    REQUIRE(app_error->reason() == "Some error occurred");
    REQUIRE(app_error->link_to_server_logs.empty());
    REQUIRE(app_error->additional_status_code);
    REQUIRE(*app_error->additional_status_code == 501);

    // Re-compose back into a Response
    err_response = AppUtils::make_apperror_response(*app_error);
    REQUIRE(err_response.http_status_code == 501);
    REQUIRE(err_response.body == "Some error occurred");
    REQUIRE(err_response.client_error_code == ErrorCodes::BadBsonParse);
    REQUIRE(err_response.custom_status_code == 0);
    REQUIRE(err_response.headers.empty());

    // Same response with client error code, but no body
    client_response.body = "";
    app_error = AppUtils::check_for_errors(client_response);
    REQUIRE(app_error);
    REQUIRE(app_error->reason() == "client error code value considered fatal");

    // Re-compose back into a Response
    err_response = AppUtils::make_apperror_response(*app_error);
    REQUIRE(err_response.http_status_code == 501);
    REQUIRE(err_response.body == "client error code value considered fatal");
    REQUIRE(err_response.client_error_code == ErrorCodes::BadBsonParse);
    REQUIRE(err_response.custom_status_code == 0);
    REQUIRE(err_response.headers.empty());

    // Valid custom status code, with body, but no json
    app::Response custom_response = {501,
                                     4999, // custom_status_code
                                     {},
                                     "Some custom error occurred"};
    app_error = AppUtils::check_for_errors(custom_response);
    REQUIRE(app_error);
    REQUIRE(app_error->code() == ErrorCodes::CustomError);
    REQUIRE(app_error->code_string() == "CustomError");
    REQUIRE(app_error->server_error.empty());
    REQUIRE(app_error->reason() == "Some custom error occurred");
    REQUIRE(app_error->link_to_server_logs.empty());
    REQUIRE(app_error->additional_status_code);
    REQUIRE(*app_error->additional_status_code == 4999);

    // Re-compose back into a Response
    err_response = AppUtils::make_apperror_response(*app_error);
    REQUIRE(err_response.http_status_code == 0);
    REQUIRE(err_response.body == "Some custom error occurred");
    REQUIRE(!err_response.client_error_code);
    REQUIRE(err_response.custom_status_code == 4999);
    REQUIRE(err_response.headers.empty());

    // Same response with custom status code, but no body
    custom_response.body = "";
    app_error = AppUtils::check_for_errors(custom_response);
    REQUIRE(app_error);
    REQUIRE(app_error->reason() == "non-zero custom status code considered fatal");

    // Re-compose back into a Response
    err_response = AppUtils::make_apperror_response(*app_error);
    REQUIRE(err_response.http_status_code == 0);
    REQUIRE(err_response.body == "non-zero custom status code considered fatal");
    REQUIRE(!err_response.client_error_code);
    REQUIRE(err_response.custom_status_code == 4999);
    REQUIRE(err_response.headers.empty());
}

// MARK: - Verify generic app utils helper functions
TEST_CASE("app: verify app utils helpers", "[sync][app][local]") {
    SECTION("split_url") {
        auto verify_good_url = [](std::string scheme, std::string server, std::string request) {
            std::string url = util::format("%1://%2%3", scheme, server, request);
            auto comp = AppUtils::split_url(url);
            REQUIRE(comp.is_ok());
            REQUIRE(comp.get_value().scheme == scheme);
            REQUIRE(comp.get_value().server == server);
            REQUIRE(comp.get_value().request == request);
        };

        verify_good_url("https", "some.host.com", "/path/to/use?some_query=do-something#fragment");
        verify_good_url("wss", "localhost:9090", "");
        verify_good_url("scheme", "user:pass@host.com", "/");
        verify_good_url("mqtt", "host", "/some/path:that?is@not*really(valid)");

        // Verify bad urls
        auto comp = AppUtils::split_url("localhost/path");
        REQUIRE(!comp.is_ok());
        comp = AppUtils::split_url("http:localhost/path");
        REQUIRE(!comp.is_ok());
        comp = AppUtils::split_url("http:/localhost/path");
        REQUIRE(!comp.is_ok());
        comp = AppUtils::split_url("https://");
        REQUIRE(!comp.is_ok());
        comp = AppUtils::split_url("http:///localhost/path");
        REQUIRE(!comp.is_ok());
        comp = AppUtils::split_url("");
        REQUIRE(!comp.is_ok());
    }

    SECTION("find_header") {
        std::map<std::string, std::string> headers1 = {{"header1", "header1-value"},
                                                       {"HEADER2", "header2-value"},
                                                       {"HeAdEr3", "header3-value"},
                                                       {"header@4", "header4-value"}};

        std::map<std::string, std::string> headers2 = {
            {"", "no-key-value"},
            {"header1", "header1-value"},
        };

        CHECK(AppUtils::find_header("", headers1) == nullptr);
        CHECK(AppUtils::find_header("header", headers1) == nullptr);
        CHECK(AppUtils::find_header("header*4", headers1) == nullptr);
        CHECK(AppUtils::find_header("header5", headers1) == nullptr);
        auto value = AppUtils::find_header("header1", headers1);
        CHECK(value != nullptr);
        CHECK(value->first == "header1");
        CHECK(value->second == "header1-value");
        value = AppUtils::find_header("HEADER1", headers1);
        CHECK(value != nullptr);
        CHECK(value->first == "header1");
        CHECK(value->second == "header1-value");
        value = AppUtils::find_header("header2", headers1);
        CHECK(value != nullptr);
        CHECK(value->first == "HEADER2");
        CHECK(value->second == "header2-value");
        value = AppUtils::find_header("hEaDeR2", headers1);
        CHECK(value != nullptr);
        CHECK(value->first == "HEADER2");
        CHECK(value->second == "header2-value");
        value = AppUtils::find_header("HEADER3", headers1);
        CHECK(value != nullptr);
        CHECK(value->first == "HeAdEr3");
        CHECK(value->second == "header3-value");
        value = AppUtils::find_header("header3", headers1);
        CHECK(value != nullptr);
        CHECK(value->first == "HeAdEr3");
        CHECK(value->second == "header3-value");
        value = AppUtils::find_header("HEADER@4", headers1);
        CHECK(value != nullptr);
        CHECK(value->first == "header@4");
        CHECK(value->second == "header4-value");
        value = AppUtils::find_header("", headers2);
        CHECK(value != nullptr);
        CHECK(value->first == "");
        CHECK(value->second == "no-key-value");
        value = AppUtils::find_header("HeAdEr1", headers2);
        CHECK(value != nullptr);
        CHECK(value->first == "header1");
        CHECK(value->second == "header1-value");
    }

    SECTION("is_success_status_code") {
        CHECK(AppUtils::is_success_status_code(0));
        for (int code = 200; code < 300; code++) {
            CHECK(AppUtils::is_success_status_code(code));
        }
        CHECK(!AppUtils::is_success_status_code(1));
        CHECK(!AppUtils::is_success_status_code(199));
        CHECK(!AppUtils::is_success_status_code(300));
        CHECK(!AppUtils::is_success_status_code(99999));
    }

    SECTION("is_redirect_status_code") {
        // Only MovedPermanently(301) and PermanentRedirect(308) return true
        CHECK(AppUtils::is_redirect_status_code(301));
        CHECK(AppUtils::is_redirect_status_code(308));
        CHECK(!AppUtils::is_redirect_status_code(0));
        CHECK(!AppUtils::is_redirect_status_code(200));
        CHECK(!AppUtils::is_redirect_status_code(300));
        CHECK(!AppUtils::is_redirect_status_code(403));
        CHECK(!AppUtils::is_redirect_status_code(99999));
    }

    SECTION("extract_redir_location") {
        auto comp = AppUtils::extract_redir_location(
            {{"Content-Type", "application/json"}, {"Location", "http://redirect.host"}});
        CHECK(comp == "http://redirect.host");
        comp = AppUtils::extract_redir_location({{"location", "http://redirect.host"}});
        CHECK(comp == "http://redirect.host");
        comp = AppUtils::extract_redir_location({{"LoCaTiOn", "http://redirect.host/"}});
        CHECK(comp == "http://redirect.host/");
        comp = AppUtils::extract_redir_location({{"LOCATION", "http://redirect.host/includes/path"}});
        CHECK(comp == "http://redirect.host/includes/path");
        comp = AppUtils::extract_redir_location({{"Content-Type", "application/json"}});
        CHECK(!comp);
        comp = AppUtils::extract_redir_location({{"some-location", "http://redirect.host"}});
        CHECK(!comp);
        comp = AppUtils::extract_redir_location({{"location", ""}});
        CHECK(!comp);
        comp = AppUtils::extract_redir_location({});
        CHECK(!comp);
        comp = AppUtils::extract_redir_location({{"location", "bad-server-url"}});
        CHECK(!comp);
    }
}

// MARK: - Login with Credentials Tests

TEST_CASE("app: login_with_credentials integration", "[sync][app][user][baas]") {
    SECTION("login") {
        TestAppSession session;
        auto app = session.app();
        app->log_out([](auto) {});

        int subscribe_processed = 0;

        auto token = app->subscribe([&subscribe_processed](auto&) {
            subscribe_processed++;
        });

        REQUIRE_FALSE(app->current_user());
        auto user = log_in(app);
        CHECK(!user->device_id().empty());
        CHECK(user->has_device_id());
        REQUIRE(app->current_user());
        CHECK(subscribe_processed == 1);

        bool processed = false;
        app->log_out([&](auto error) {
            REQUIRE_FALSE(error);
            processed = true;
        });
        REQUIRE_FALSE(app->current_user());
        CHECK(processed);
        CHECK(subscribe_processed == 2);

        app->unsubscribe(token);
    }
}

// MARK: - UsernamePasswordProviderClient Tests

TEST_CASE("app: UsernamePasswordProviderClient integration", "[sync][app][user][baas]") {
    const std::string base_url = get_base_url();
    AutoVerifiedEmailCredentials creds;
    auto email = creds.email;
    auto password = creds.password;

    TestAppSession session;
    auto app = session.app();
    auto client = app->provider_client<App::UsernamePasswordProviderClient>();

    bool processed = false;

    client.register_email(email, password, [&](Optional<AppError> error) {
        CAPTURE(email);
        CAPTURE(password);
        REQUIRE_FALSE(error); // first registration success
    });

    SECTION("double registration should fail") {
        client.register_email(email, password, [&](Optional<AppError> error) {
            // Error returned states the account has already been created
            REQUIRE(error);
            CHECK(error->reason() == "name already in use");
            CHECK(error->code() == ErrorCodes::AccountNameInUse);
            CHECK(!error->link_to_server_logs.empty());
            CHECK(error->link_to_server_logs.find(base_url) != std::string::npos);
            processed = true;
        });
        CHECK(processed);
    }

    SECTION("double registration should fail") {
        // the server registration function will reject emails that do not contain "realm_tests_do_autoverify"
        std::string email_to_reject = util::format("%1@%2.com", random_string(10), random_string(10));
        client.register_email(email_to_reject, password, [&](Optional<AppError> error) {
            REQUIRE(error);
            CHECK(error->reason() == util::format("failed to confirm user \"%1\"", email_to_reject));
            CHECK(error->code() == ErrorCodes::BadRequest);
            processed = true;
        });
        CHECK(processed);
    }

    SECTION("can login with registered account") {
        auto user = log_in(app, creds);
        CHECK(user->user_profile().email() == email);
    }

    SECTION("cannot login with wrong password") {
        app->log_in_with_credentials(AppCredentials::username_password(email, "boogeyman"),
                                     [&](std::shared_ptr<realm::SyncUser> user, Optional<AppError> error) {
                                         CHECK(!user);
                                         REQUIRE(error);
                                         REQUIRE(error->code() == ErrorCodes::InvalidPassword);
                                         processed = true;
                                     });
        CHECK(processed);
    }

    SECTION("confirm user") {
        client.confirm_user("a_token", "a_token_id", [&](Optional<AppError> error) {
            REQUIRE(error);
            CHECK(error->reason() == "invalid token data");
            processed = true;
        });
        CHECK(processed);
    }

    SECTION("resend confirmation email") {
        client.resend_confirmation_email(email, [&](Optional<AppError> error) {
            REQUIRE(error);
            CHECK(error->reason() == "already confirmed");
            processed = true;
        });
        CHECK(processed);
    }

    SECTION("reset password invalid tokens") {
        client.reset_password(password, "token_sample", "token_id_sample", [&](Optional<AppError> error) {
            REQUIRE(error);
            CHECK(error->reason() == "invalid token data");
            CHECK(!error->link_to_server_logs.empty());
            CHECK(error->link_to_server_logs.find(base_url) != std::string::npos);
            processed = true;
        });
        CHECK(processed);
    }

    SECTION("reset password function success") {
        // the imported test app will accept password reset if the password contains "realm_tests_do_reset" via a
        // function
        std::string accepted_new_password = util::format("realm_tests_do_reset%1", random_string(10));
        client.call_reset_password_function(email, accepted_new_password, {}, [&](Optional<AppError> error) {
            REQUIRE_FALSE(error);
            processed = true;
        });
        CHECK(processed);
    }

    SECTION("reset password function failure") {
        std::string rejected_password = util::format("%1", random_string(10));
        client.call_reset_password_function(email, rejected_password, {"foo", "bar"}, [&](Optional<AppError> error) {
            REQUIRE(error);
            CHECK(error->reason() == util::format("failed to reset password for user \"%1\"", email));
            CHECK(error->is_service_error());
            processed = true;
        });
        CHECK(processed);
    }

    SECTION("reset password function for invalid user fails") {
        client.call_reset_password_function(util::format("%1@%2.com", random_string(5), random_string(5)), password,
                                            {"foo", "bar"}, [&](Optional<AppError> error) {
                                                REQUIRE(error);
                                                CHECK(error->reason() == "user not found");
                                                CHECK(error->is_service_error());
                                                CHECK(error->code() == ErrorCodes::UserNotFound);
                                                processed = true;
                                            });
        CHECK(processed);
    }

    SECTION("retry custom confirmation") {
        client.retry_custom_confirmation(email, [&](Optional<AppError> error) {
            REQUIRE(error);
            CHECK(error->reason() == "already confirmed");
            processed = true;
        });
        CHECK(processed);
    }

    SECTION("retry custom confirmation for invalid user fails") {
        client.retry_custom_confirmation(util::format("%1@%2.com", random_string(5), random_string(5)),
                                         [&](Optional<AppError> error) {
                                             REQUIRE(error);
                                             CHECK(error->reason() == "user not found");
                                             CHECK(error->is_service_error());
                                             CHECK(error->code() == ErrorCodes::UserNotFound);
                                             processed = true;
                                         });
        CHECK(processed);
    }

    SECTION("log in, remove, log in") {
        app->remove_user(app->current_user(), [](auto) {});
        CHECK(app->all_users().size() == 0);
        CHECK(app->current_user() == nullptr);

        auto user = log_in(app, AppCredentials::username_password(email, password));
        CHECK(user->user_profile().email() == email);
        CHECK(user->state() == SyncUser::State::LoggedIn);

        app->remove_user(user, [&](Optional<AppError> error) {
            REQUIRE_FALSE(error);
        });
        CHECK(user->state() == SyncUser::State::Removed);

        log_in(app, AppCredentials::username_password(email, password));
        CHECK(user->state() == SyncUser::State::Removed);
        CHECK(app->current_user() != user);
        user = app->current_user();
        CHECK(user->user_profile().email() == email);
        CHECK(user->state() == SyncUser::State::LoggedIn);

        app->remove_user(user, [&](Optional<AppError> error) {
            REQUIRE(!error);
            CHECK(app->all_users().size() == 0);
            processed = true;
        });

        CHECK(user->state() == SyncUser::State::Removed);
        CHECK(processed);
        CHECK(app->all_users().size() == 0);
    }
}

// MARK: - UserAPIKeyProviderClient Tests

TEST_CASE("app: UserAPIKeyProviderClient integration", "[sync][app][api key][baas]") {
    TestAppSession session;
    auto app = session.app();
    auto client = app->provider_client<App::UserAPIKeyProviderClient>();

    bool processed = false;
    App::UserAPIKey api_key;

    SECTION("api-key") {
        std::shared_ptr<SyncUser> logged_in_user = app->current_user();
        auto api_key_name = util::format("%1", random_string(15));
        client.create_api_key(api_key_name, logged_in_user,
                              [&](App::UserAPIKey user_api_key, Optional<AppError> error) {
                                  REQUIRE_FALSE(error);
                                  CHECK(user_api_key.name == api_key_name);
                                  api_key = user_api_key;
                              });

        client.fetch_api_key(api_key.id, logged_in_user, [&](App::UserAPIKey user_api_key, Optional<AppError> error) {
            REQUIRE_FALSE(error);
            CHECK(user_api_key.name == api_key_name);
            CHECK(user_api_key.id == api_key.id);
        });

        client.fetch_api_keys(logged_in_user, [&](std::vector<App::UserAPIKey> api_keys, Optional<AppError> error) {
            CHECK(api_keys.size() == 1);
            for (auto key : api_keys) {
                CHECK(key.id.to_string() == api_key.id.to_string());
                CHECK(api_key.name == api_key_name);
                CHECK(key.id == api_key.id);
            }
            REQUIRE_FALSE(error);
        });

        client.enable_api_key(api_key.id, logged_in_user, [&](Optional<AppError> error) {
            REQUIRE_FALSE(error);
        });

        client.fetch_api_key(api_key.id, logged_in_user, [&](App::UserAPIKey user_api_key, Optional<AppError> error) {
            REQUIRE_FALSE(error);
            CHECK(user_api_key.disabled == false);
            CHECK(user_api_key.name == api_key_name);
            CHECK(user_api_key.id == api_key.id);
        });

        client.disable_api_key(api_key.id, logged_in_user, [&](Optional<AppError> error) {
            REQUIRE_FALSE(error);
        });

        client.fetch_api_key(api_key.id, logged_in_user, [&](App::UserAPIKey user_api_key, Optional<AppError> error) {
            REQUIRE_FALSE(error);
            CHECK(user_api_key.disabled == true);
            CHECK(user_api_key.name == api_key_name);
        });

        client.delete_api_key(api_key.id, logged_in_user, [&](Optional<AppError> error) {
            REQUIRE_FALSE(error);
        });

        client.fetch_api_key(api_key.id, logged_in_user, [&](App::UserAPIKey user_api_key, Optional<AppError> error) {
            CHECK(user_api_key.name == "");
            CHECK(error);
            processed = true;
        });

        CHECK(processed);
    }

    SECTION("api-key without a user") {
        std::shared_ptr<SyncUser> no_user = nullptr;
        auto api_key_name = util::format("%1", random_string(15));
        client.create_api_key(api_key_name, no_user, [&](App::UserAPIKey user_api_key, Optional<AppError> error) {
            REQUIRE(error);
            CHECK(error->is_service_error());
            CHECK(error->reason() == "must authenticate first");
            CHECK(user_api_key.name == "");
        });

        client.fetch_api_key(api_key.id, no_user, [&](App::UserAPIKey user_api_key, Optional<AppError> error) {
            REQUIRE(error);
            CHECK(error->is_service_error());
            CHECK(error->reason() == "must authenticate first");
            CHECK(user_api_key.name == "");
        });

        client.fetch_api_keys(no_user, [&](std::vector<App::UserAPIKey> api_keys, Optional<AppError> error) {
            REQUIRE(error);
            CHECK(error->is_service_error());
            CHECK(error->reason() == "must authenticate first");
            CHECK(api_keys.size() == 0);
        });

        client.enable_api_key(api_key.id, no_user, [&](Optional<AppError> error) {
            REQUIRE(error);
            CHECK(error->is_service_error());
            CHECK(error->reason() == "must authenticate first");
        });

        client.fetch_api_key(api_key.id, no_user, [&](App::UserAPIKey user_api_key, Optional<AppError> error) {
            REQUIRE(error);
            CHECK(error->is_service_error());
            CHECK(error->reason() == "must authenticate first");
            CHECK(user_api_key.name == "");
        });

        client.disable_api_key(api_key.id, no_user, [&](Optional<AppError> error) {
            REQUIRE(error);
            CHECK(error->is_service_error());
            CHECK(error->reason() == "must authenticate first");
        });

        client.fetch_api_key(api_key.id, no_user, [&](App::UserAPIKey user_api_key, Optional<AppError> error) {
            REQUIRE(error);
            CHECK(error->is_service_error());
            CHECK(error->reason() == "must authenticate first");
            CHECK(user_api_key.name == "");
        });

        client.delete_api_key(api_key.id, no_user, [&](Optional<AppError> error) {
            REQUIRE(error);
            CHECK(error->is_service_error());
            CHECK(error->reason() == "must authenticate first");
        });

        client.fetch_api_key(api_key.id, no_user, [&](App::UserAPIKey user_api_key, Optional<AppError> error) {
            CHECK(user_api_key.name == "");
            REQUIRE(error);
            CHECK(error->is_service_error());
            CHECK(error->reason() == "must authenticate first");
            processed = true;
        });
        CHECK(processed);
    }

    SECTION("api-key against the wrong user") {
        std::shared_ptr<SyncUser> first_user = app->current_user();
        create_user_and_log_in(app);
        std::shared_ptr<SyncUser> second_user = app->current_user();
        REQUIRE(first_user != second_user);
        auto api_key_name = util::format("%1", random_string(15));
        App::UserAPIKey api_key;
        App::UserAPIKeyProviderClient provider = app->provider_client<App::UserAPIKeyProviderClient>();

        provider.create_api_key(api_key_name, first_user,
                                [&](App::UserAPIKey user_api_key, Optional<AppError> error) {
                                    REQUIRE_FALSE(error);
                                    CHECK(user_api_key.name == api_key_name);
                                    api_key = user_api_key;
                                });

        provider.fetch_api_key(api_key.id, first_user, [&](App::UserAPIKey user_api_key, Optional<AppError> error) {
            REQUIRE_FALSE(error);
            CHECK(user_api_key.name == api_key_name);
            CHECK(user_api_key.id.to_string() == user_api_key.id.to_string());
        });

        provider.fetch_api_key(api_key.id, second_user, [&](App::UserAPIKey user_api_key, Optional<AppError> error) {
            REQUIRE(error);
            CHECK(error->reason() == "API key not found");
            CHECK(error->is_service_error());
            CHECK(error->code() == ErrorCodes::APIKeyNotFound);
            CHECK(user_api_key.name == "");
        });

        provider.fetch_api_keys(first_user, [&](std::vector<App::UserAPIKey> api_keys, Optional<AppError> error) {
            CHECK(api_keys.size() == 1);
            for (auto api_key : api_keys) {
                CHECK(api_key.name == api_key_name);
            }
            REQUIRE_FALSE(error);
        });

        provider.fetch_api_keys(second_user, [&](std::vector<App::UserAPIKey> api_keys, Optional<AppError> error) {
            CHECK(api_keys.size() == 0);
            REQUIRE_FALSE(error);
        });

        provider.enable_api_key(api_key.id, first_user, [&](Optional<AppError> error) {
            REQUIRE_FALSE(error);
        });

        provider.enable_api_key(api_key.id, second_user, [&](Optional<AppError> error) {
            REQUIRE(error);
            CHECK(error->reason() == "API key not found");
            CHECK(error->is_service_error());
            CHECK(error->code() == ErrorCodes::APIKeyNotFound);
        });

        provider.fetch_api_key(api_key.id, first_user, [&](App::UserAPIKey user_api_key, Optional<AppError> error) {
            REQUIRE_FALSE(error);
            CHECK(user_api_key.disabled == false);
            CHECK(user_api_key.name == api_key_name);
        });

        provider.fetch_api_key(api_key.id, second_user, [&](App::UserAPIKey user_api_key, Optional<AppError> error) {
            REQUIRE(error);
            CHECK(user_api_key.name == "");
            CHECK(error->reason() == "API key not found");
            CHECK(error->is_service_error());
            CHECK(error->code() == ErrorCodes::APIKeyNotFound);
        });

        provider.disable_api_key(api_key.id, first_user, [&](Optional<AppError> error) {
            REQUIRE_FALSE(error);
        });

        provider.disable_api_key(api_key.id, second_user, [&](Optional<AppError> error) {
            REQUIRE(error);
            CHECK(error->reason() == "API key not found");
            CHECK(error->is_service_error());
            CHECK(error->code() == ErrorCodes::APIKeyNotFound);
        });

        provider.fetch_api_key(api_key.id, first_user, [&](App::UserAPIKey user_api_key, Optional<AppError> error) {
            REQUIRE_FALSE(error);
            CHECK(user_api_key.disabled == true);
            CHECK(user_api_key.name == api_key_name);
        });

        provider.fetch_api_key(api_key.id, second_user, [&](App::UserAPIKey user_api_key, Optional<AppError> error) {
            REQUIRE(error);
            CHECK(user_api_key.name == "");
            CHECK(error->reason() == "API key not found");
            CHECK(error->is_service_error());
            CHECK(error->code() == ErrorCodes::APIKeyNotFound);
        });

        provider.delete_api_key(api_key.id, second_user, [&](Optional<AppError> error) {
            REQUIRE(error);
            CHECK(error->reason() == "API key not found");
            CHECK(error->is_service_error());
            CHECK(error->code() == ErrorCodes::APIKeyNotFound);
        });

        provider.delete_api_key(api_key.id, first_user, [&](Optional<AppError> error) {
            REQUIRE_FALSE(error);
        });

        provider.fetch_api_key(api_key.id, first_user, [&](App::UserAPIKey user_api_key, Optional<AppError> error) {
            CHECK(user_api_key.name == "");
            REQUIRE(error);
            CHECK(error->reason() == "API key not found");
            CHECK(error->is_service_error());
            CHECK(error->code() == ErrorCodes::APIKeyNotFound);
            processed = true;
        });

        provider.fetch_api_key(api_key.id, second_user, [&](App::UserAPIKey user_api_key, Optional<AppError> error) {
            CHECK(user_api_key.name == "");
            REQUIRE(error);
            CHECK(error->reason() == "API key not found");
            CHECK(error->is_service_error());
            CHECK(error->code() == ErrorCodes::APIKeyNotFound);
            processed = true;
        });

        CHECK(processed);
    }
}

// MARK: - Auth Providers Function Tests

TEST_CASE("app: auth providers function integration", "[sync][app][user][baas]") {
    TestAppSession session;
    auto app = session.app();

    SECTION("auth providers function integration") {
        bson::BsonDocument function_params{{"realmCustomAuthFuncUserId", "123456"}};
        auto credentials = AppCredentials::function(function_params);
        auto user = log_in(app, credentials);
        REQUIRE(user->identities()[0].provider_type == IdentityProviderFunction);
    }
}

// MARK: - Link User Tests

TEST_CASE("app: Linking user identities", "[sync][app][user][baas]") {
    TestAppSession session;
    auto app = session.app();
    auto user = log_in(app);

    AutoVerifiedEmailCredentials creds;
    app->provider_client<App::UsernamePasswordProviderClient>().register_email(creds.email, creds.password,
                                                                               [&](Optional<AppError> error) {
                                                                                   REQUIRE_FALSE(error);
                                                                               });

    SECTION("anonymous users are reused before they are linked to an identity") {
        REQUIRE(user == log_in(app));
    }

    SECTION("linking a user adds that identity to the user") {
        REQUIRE(user->identities().size() == 1);
        CHECK(user->identities()[0].provider_type == IdentityProviderAnonymous);

        app->link_user(user, creds, [&](std::shared_ptr<SyncUser> user2, Optional<AppError> error) {
            REQUIRE_FALSE(error);
            REQUIRE(user == user2);
            REQUIRE(user->identities().size() == 2);
            CHECK(user->identities()[0].provider_type == IdentityProviderAnonymous);
            CHECK(user->identities()[1].provider_type == IdentityProviderUsernamePassword);
        });
    }

    SECTION("linking an identity makes the user no longer returned by anonymous logins") {
        app->link_user(user, creds, [&](std::shared_ptr<SyncUser>, Optional<AppError> error) {
            REQUIRE_FALSE(error);
        });
        auto user2 = log_in(app);
        REQUIRE(user != user2);
    }

    SECTION("existing users are reused when logging in via linked identities") {
        app->link_user(user, creds, [](std::shared_ptr<SyncUser>, Optional<AppError> error) {
            REQUIRE_FALSE(error);
        });
        app->log_out([](auto error) {
            REQUIRE_FALSE(error);
        });
        REQUIRE(user->state() == SyncUser::State::LoggedOut);
        // Should give us the same user instance despite logging in with a
        // different identity
        REQUIRE(user == log_in(app, creds));
        REQUIRE(user->state() == SyncUser::State::LoggedIn);
    }
}

// MARK: - Delete User Tests

TEST_CASE("app: delete anonymous user integration", "[sync][app][user][baas]") {
    TestAppSession session;
    auto app = session.app();

    SECTION("delete user expect success") {
        CHECK(app->all_users().size() == 1);

        // Log in user 1
        auto user_a = app->current_user();
        CHECK(user_a->state() == SyncUser::State::LoggedIn);
        app->delete_user(user_a, [&](Optional<app::AppError> error) {
            REQUIRE_FALSE(error);
            // a logged out anon user will be marked as Removed, not LoggedOut
            CHECK(user_a->state() == SyncUser::State::Removed);
        });
        CHECK(app->all_users().empty());
        CHECK(app->current_user() == nullptr);

        app->delete_user(user_a, [&](Optional<app::AppError> error) {
            CHECK(error->reason() == "User must be logged in to be deleted.");
            CHECK(app->all_users().size() == 0);
        });

        // Log in user 2
        auto user_b = log_in(app);
        CHECK(app->current_user() == user_b);
        CHECK(user_b->state() == SyncUser::State::LoggedIn);
        CHECK(app->all_users().size() == 1);

        app->delete_user(user_b, [&](Optional<app::AppError> error) {
            REQUIRE_FALSE(error);
            CHECK(app->all_users().size() == 0);
        });

        CHECK(app->current_user() == nullptr);

        // check both handles are no longer valid
        CHECK(user_a->state() == SyncUser::State::Removed);
        CHECK(user_b->state() == SyncUser::State::Removed);
    }
}

TEST_CASE("app: delete user with credentials integration", "[sync][app][user][baas]") {
    TestAppSession session;
    auto app = session.app();
    app->remove_user(app->current_user(), [](auto) {});

    SECTION("log in and delete") {
        CHECK(app->all_users().size() == 0);
        CHECK(app->current_user() == nullptr);

        auto credentials = create_user_and_log_in(app);
        auto user = app->current_user();

        CHECK(app->current_user() == user);
        CHECK(user->state() == SyncUser::State::LoggedIn);
        app->delete_user(user, [&](Optional<app::AppError> error) {
            REQUIRE_FALSE(error);
            CHECK(app->all_users().size() == 0);
        });
        CHECK(user->state() == SyncUser::State::Removed);
        CHECK(app->current_user() == nullptr);

        app->log_in_with_credentials(credentials, [](std::shared_ptr<SyncUser> user, util::Optional<AppError> error) {
            CHECK(!user);
            REQUIRE(error);
            REQUIRE(error->code() == ErrorCodes::InvalidPassword);
        });
        CHECK(app->current_user() == nullptr);

        CHECK(app->all_users().size() == 0);
        app->delete_user(user, [](Optional<app::AppError> err) {
            CHECK(err->code() > 0);
        });

        CHECK(app->current_user() == nullptr);
        CHECK(app->all_users().size() == 0);
        CHECK(user->state() == SyncUser::State::Removed);
    }
}

// MARK: - Call Function Tests

TEST_CASE("app: call function", "[sync][app][function][baas]") {
    TestAppSession session;
    auto app = session.app();

    bson::BsonArray toSum(5);
    std::iota(toSum.begin(), toSum.end(), static_cast<int64_t>(1));
    const auto checkFn = [](Optional<int64_t>&& sum, Optional<AppError>&& error) {
        REQUIRE(!error);
        CHECK(*sum == 15);
    };
    app->call_function<int64_t>("sumFunc", toSum, checkFn);
    app->call_function<int64_t>(app->current_user(), "sumFunc", toSum, checkFn);
}

// MARK: - Remote Mongo Client Tests

TEST_CASE("app: remote mongo client", "[sync][app][mongo][baas]") {
    TestAppSession session;
    auto app = session.app();

    auto remote_client = app->current_user()->mongo_client("BackingDB");
    auto app_session = get_runtime_app_session();
    auto db = remote_client.db(app_session.config.mongo_dbname);
    auto dog_collection = db["Dog"];
    auto cat_collection = db["Cat"];
    auto person_collection = db["Person"];

    bson::BsonDocument dog_document{{"name", "fido"}, {"breed", "king charles"}};

    bson::BsonDocument dog_document2{{"name", "bob"}, {"breed", "french bulldog"}};

    auto dog3_object_id = ObjectId::gen();
    bson::BsonDocument dog_document3{
        {"_id", dog3_object_id},
        {"name", "petunia"},
        {"breed", "french bulldog"},
    };

    auto cat_id_string = random_string(10);
    bson::BsonDocument cat_document{
        {"_id", cat_id_string},
        {"name", "luna"},
        {"breed", "scottish fold"},
    };

    bson::BsonDocument person_document{
        {"firstName", "John"},
        {"lastName", "Johnson"},
        {"age", 30},
    };

    bson::BsonDocument person_document2{
        {"firstName", "Bob"},
        {"lastName", "Johnson"},
        {"age", 30},
    };

    bson::BsonDocument bad_document{{"bad", "value"}};

    dog_collection.delete_many(dog_document, [&](uint64_t, Optional<AppError> error) {
        REQUIRE_FALSE(error);
    });

    dog_collection.delete_many(dog_document2, [&](uint64_t, Optional<AppError> error) {
        REQUIRE_FALSE(error);
    });

    dog_collection.delete_many({}, [&](uint64_t, Optional<AppError> error) {
        REQUIRE_FALSE(error);
    });

    dog_collection.delete_many(person_document, [&](uint64_t, Optional<AppError> error) {
        REQUIRE_FALSE(error);
    });

    dog_collection.delete_many(person_document2, [&](uint64_t, Optional<AppError> error) {
        REQUIRE_FALSE(error);
    });

    SECTION("insert") {
        bool processed = false;
        ObjectId dog_object_id;
        ObjectId dog2_object_id;

        dog_collection.insert_one_bson(bad_document, [&](Optional<bson::Bson> bson, Optional<AppError> error) {
            CHECK(error);
            CHECK(!bson);
        });

        dog_collection.insert_one_bson(dog_document3, [&](Optional<bson::Bson> value, Optional<AppError> error) {
            REQUIRE_FALSE(error);
            auto bson = static_cast<bson::BsonDocument>(*value);
            CHECK(static_cast<ObjectId>(bson["insertedId"]) == dog3_object_id);
        });

        cat_collection.insert_one_bson(cat_document, [&](Optional<bson::Bson> value, Optional<AppError> error) {
            REQUIRE_FALSE(error);
            auto bson = static_cast<bson::BsonDocument>(*value);
            CHECK(static_cast<std::string>(bson["insertedId"]) == cat_id_string);
        });

        dog_collection.delete_many({}, [&](uint64_t, Optional<AppError> error) {
            REQUIRE_FALSE(error);
        });

        cat_collection.delete_one(cat_document, [&](uint64_t, Optional<AppError> error) {
            REQUIRE_FALSE(error);
        });

        dog_collection.insert_one(bad_document, [&](Optional<bson::Bson> object_id, Optional<AppError> error) {
            CHECK(error);
            CHECK(!object_id);
        });

        dog_collection.insert_one(dog_document, [&](Optional<bson::Bson> object_id, Optional<AppError> error) {
            REQUIRE_FALSE(error);
            CHECK((*object_id).to_string() != "");
            dog_object_id = static_cast<ObjectId>(*object_id);
        });

        dog_collection.insert_one(dog_document2, [&](Optional<bson::Bson> object_id, Optional<AppError> error) {
            REQUIRE_FALSE(error);
            CHECK((*object_id).to_string() != "");
            dog2_object_id = static_cast<ObjectId>(*object_id);
        });

        dog_collection.insert_one(dog_document3, [&](Optional<bson::Bson> object_id, Optional<AppError> error) {
            REQUIRE_FALSE(error);
            CHECK(object_id->type() == bson::Bson::Type::ObjectId);
            CHECK(static_cast<ObjectId>(*object_id) == dog3_object_id);
        });

        cat_collection.insert_one(cat_document, [&](Optional<bson::Bson> object_id, Optional<AppError> error) {
            REQUIRE_FALSE(error);
            CHECK(object_id->type() == bson::Bson::Type::String);
            CHECK(static_cast<std::string>(*object_id) == cat_id_string);
        });

        person_document["dogs"] = bson::BsonArray({dog_object_id, dog2_object_id, dog3_object_id});
        person_collection.insert_one(person_document, [&](Optional<bson::Bson> object_id, Optional<AppError> error) {
            REQUIRE_FALSE(error);
            CHECK((*object_id).to_string() != "");
        });

        dog_collection.delete_many({}, [&](uint64_t, Optional<AppError> error) {
            REQUIRE_FALSE(error);
        });

        cat_collection.delete_one(cat_document, [&](uint64_t, Optional<AppError> error) {
            REQUIRE_FALSE(error);
        });

        bson::BsonArray documents{
            dog_document,
            dog_document2,
            dog_document3,
        };

        dog_collection.insert_many_bson(documents, [&](Optional<bson::Bson> value, Optional<AppError> error) {
            REQUIRE_FALSE(error);
            auto bson = static_cast<bson::BsonDocument>(*value);
            auto insertedIds = static_cast<bson::BsonArray>(bson["insertedIds"]);
        });

        dog_collection.delete_many({}, [&](uint64_t, Optional<AppError> error) {
            REQUIRE_FALSE(error);
        });

        dog_collection.insert_many(documents, [&](bson::BsonArray inserted_docs, Optional<AppError> error) {
            REQUIRE_FALSE(error);
            CHECK(inserted_docs.size() == 3);
            CHECK(inserted_docs[0].type() == bson::Bson::Type::ObjectId);
            CHECK(inserted_docs[1].type() == bson::Bson::Type::ObjectId);
            CHECK(inserted_docs[2].type() == bson::Bson::Type::ObjectId);
            CHECK(static_cast<ObjectId>(inserted_docs[2]) == dog3_object_id);
            processed = true;
        });

        CHECK(processed);
    }

    SECTION("find") {
        bool processed = false;

        dog_collection.find(dog_document, [&](Optional<bson::BsonArray> document_array, Optional<AppError> error) {
            REQUIRE_FALSE(error);
            CHECK((*document_array).size() == 0);
        });

        dog_collection.find_bson(dog_document, {}, [&](Optional<bson::Bson> bson, Optional<AppError> error) {
            REQUIRE_FALSE(error);
            CHECK(static_cast<bson::BsonArray>(*bson).size() == 0);
        });

        dog_collection.find_one(dog_document, [&](Optional<bson::BsonDocument> document, Optional<AppError> error) {
            REQUIRE_FALSE(error);
            CHECK(!document);
        });

        dog_collection.find_one_bson(dog_document, {}, [&](Optional<bson::Bson> bson, Optional<AppError> error) {
            REQUIRE_FALSE(error);
            CHECK((!bson || bson::holds_alternative<util::None>(*bson)));
        });

        ObjectId dog_object_id;
        ObjectId dog2_object_id;

        dog_collection.insert_one(dog_document, [&](Optional<bson::Bson> object_id, Optional<AppError> error) {
            REQUIRE_FALSE(error);
            CHECK((*object_id).to_string() != "");
            dog_object_id = static_cast<ObjectId>(*object_id);
        });

        dog_collection.insert_one(dog_document2, [&](Optional<bson::Bson> object_id, Optional<AppError> error) {
            REQUIRE_FALSE(error);
            CHECK((*object_id).to_string() != "");
            dog2_object_id = static_cast<ObjectId>(*object_id);
        });

        person_document["dogs"] = bson::BsonArray({dog_object_id, dog2_object_id});
        person_collection.insert_one(person_document, [&](Optional<bson::Bson> object_id, Optional<AppError> error) {
            REQUIRE_FALSE(error);
            CHECK((*object_id).to_string() != "");
        });

        dog_collection.find(dog_document, [&](Optional<bson::BsonArray> documents, Optional<AppError> error) {
            REQUIRE_FALSE(error);
            CHECK((*documents).size() == 1);
        });

        dog_collection.find_bson(dog_document, {}, [&](Optional<bson::Bson> bson, Optional<AppError> error) {
            REQUIRE_FALSE(error);
            CHECK(static_cast<bson::BsonArray>(*bson).size() == 1);
        });

        person_collection.find(person_document, [&](Optional<bson::BsonArray> documents, Optional<AppError> error) {
            REQUIRE_FALSE(error);
            CHECK((*documents).size() == 1);
        });

        MongoCollection::FindOptions options{
            2,                                                         // document limit
            Optional<bson::BsonDocument>({{"name", 1}, {"breed", 1}}), // project
            Optional<bson::BsonDocument>({{"breed", 1}})               // sort
        };

        dog_collection.find(dog_document, options,
                            [&](Optional<bson::BsonArray> document_array, Optional<AppError> error) {
                                REQUIRE_FALSE(error);
                                CHECK((*document_array).size() == 1);
                            });

        dog_collection.find({{"name", "fido"}}, options,
                            [&](Optional<bson::BsonArray> document_array, Optional<AppError> error) {
                                REQUIRE_FALSE(error);
                                CHECK((*document_array).size() == 1);
                                auto king_charles = static_cast<bson::BsonDocument>((*document_array)[0]);
                                CHECK(king_charles["breed"] == "king charles");
                            });

        dog_collection.find_one(dog_document, [&](Optional<bson::BsonDocument> document, Optional<AppError> error) {
            REQUIRE_FALSE(error);
            auto name = (*document)["name"];
            CHECK(name == "fido");
        });

        dog_collection.find_one(dog_document, options,
                                [&](Optional<bson::BsonDocument> document, Optional<AppError> error) {
                                    REQUIRE_FALSE(error);
                                    auto name = (*document)["name"];
                                    CHECK(name == "fido");
                                });

        dog_collection.find_one_bson(dog_document, options, [&](Optional<bson::Bson> bson, Optional<AppError> error) {
            REQUIRE_FALSE(error);
            auto name = (static_cast<bson::BsonDocument>(*bson))["name"];
            CHECK(name == "fido");
        });

        dog_collection.find(dog_document, [&](Optional<bson::BsonArray> documents, Optional<AppError> error) {
            REQUIRE_FALSE(error);
            CHECK((*documents).size() == 1);
        });

        dog_collection.find_one_and_delete(dog_document,
                                           [&](Optional<bson::BsonDocument> document, Optional<AppError> error) {
                                               REQUIRE_FALSE(error);
                                               REQUIRE(document);
                                           });

        dog_collection.find_one_and_delete({{}},
                                           [&](Optional<bson::BsonDocument> document, Optional<AppError> error) {
                                               REQUIRE_FALSE(error);
                                               REQUIRE(document);
                                           });

        dog_collection.find_one_and_delete({{"invalid", "key"}},
                                           [&](Optional<bson::BsonDocument> document, Optional<AppError> error) {
                                               REQUIRE_FALSE(error);
                                               CHECK(!document);
                                           });

        dog_collection.find_one_and_delete_bson({{"invalid", "key"}}, {},
                                                [&](Optional<bson::Bson> bson, Optional<AppError> error) {
                                                    REQUIRE_FALSE(error);
                                                    CHECK((!bson || bson::holds_alternative<util::None>(*bson)));
                                                });

        dog_collection.find(dog_document, [&](Optional<bson::BsonArray> documents, Optional<AppError> error) {
            REQUIRE_FALSE(error);
            CHECK((*documents).size() == 0);
            processed = true;
        });

        CHECK(processed);
    }

    SECTION("count and aggregate") {
        bool processed = false;

        ObjectId dog_object_id;
        ObjectId dog2_object_id;

        dog_collection.insert_one(dog_document, [&](Optional<bson::Bson> object_id, Optional<AppError> error) {
            REQUIRE_FALSE(error);
            CHECK((*object_id).to_string() != "");
        });

        dog_collection.insert_one(dog_document, [&](Optional<bson::Bson> object_id, Optional<AppError> error) {
            REQUIRE_FALSE(error);
            CHECK((*object_id).to_string() != "");
            dog_object_id = static_cast<ObjectId>(*object_id);
        });

        dog_collection.insert_one(dog_document2, [&](Optional<bson::Bson> object_id, Optional<AppError> error) {
            REQUIRE_FALSE(error);
            CHECK((*object_id).to_string() != "");
            dog2_object_id = static_cast<ObjectId>(*object_id);
        });

        person_document["dogs"] = bson::BsonArray({dog_object_id, dog2_object_id});
        person_collection.insert_one(person_document, [&](Optional<bson::Bson> object_id, Optional<AppError> error) {
            REQUIRE_FALSE(error);
            CHECK((*object_id).to_string() != "");
        });

        bson::BsonDocument match{{"$match", bson::BsonDocument({{"name", "fido"}})}};

        bson::BsonDocument group{{"$group", bson::BsonDocument({{"_id", "$name"}})}};

        bson::BsonArray pipeline{match, group};

        dog_collection.aggregate(pipeline, [&](Optional<bson::BsonArray> documents, Optional<AppError> error) {
            REQUIRE_FALSE(error);
            CHECK((*documents).size() == 1);
        });

        dog_collection.aggregate_bson(pipeline, [&](Optional<bson::Bson> bson, Optional<AppError> error) {
            REQUIRE_FALSE(error);
            CHECK(static_cast<bson::BsonArray>(*bson).size() == 1);
        });

        dog_collection.count({{"breed", "king charles"}}, [&](uint64_t count, Optional<AppError> error) {
            REQUIRE_FALSE(error);
            CHECK(count == 2);
        });

        dog_collection.count_bson({{"breed", "king charles"}}, 0,
                                  [&](Optional<bson::Bson> bson, Optional<AppError> error) {
                                      REQUIRE_FALSE(error);
                                      CHECK(static_cast<int64_t>(*bson) == 2);
                                  });

        dog_collection.count({{"breed", "french bulldog"}}, [&](uint64_t count, Optional<AppError> error) {
            REQUIRE_FALSE(error);
            CHECK(count == 1);
        });

        dog_collection.count({{"breed", "king charles"}}, 1, [&](uint64_t count, Optional<AppError> error) {
            REQUIRE_FALSE(error);
            CHECK(count == 1);
        });

        person_collection.count(
            {{"firstName", "John"}, {"lastName", "Johnson"}, {"age", bson::BsonDocument({{"$gt", 25}})}}, 1,
            [&](uint64_t count, Optional<AppError> error) {
                REQUIRE_FALSE(error);
                CHECK(count == 1);
                processed = true;
            });

        CHECK(processed);
    }

    SECTION("find and update") {
        bool processed = false;

        MongoCollection::FindOneAndModifyOptions find_and_modify_options{
            Optional<bson::BsonDocument>({{"name", 1}, {"breed", 1}}), // project
            Optional<bson::BsonDocument>({{"name", 1}}),               // sort,
            true,                                                      // upsert
            true                                                       // return new doc
        };

        dog_collection.find_one_and_update(dog_document, dog_document2,
                                           [&](Optional<bson::BsonDocument> document, Optional<AppError> error) {
                                               REQUIRE_FALSE(error);
                                               CHECK(!document);
                                           });

        dog_collection.insert_one(dog_document, [&](Optional<bson::Bson> object_id, Optional<AppError> error) {
            REQUIRE_FALSE(error);
            CHECK((*object_id).to_string() != "");
        });

        dog_collection.find_one_and_update(dog_document, dog_document2, find_and_modify_options,
                                           [&](Optional<bson::BsonDocument> document, Optional<AppError> error) {
                                               REQUIRE_FALSE(error);
                                               auto breed = static_cast<std::string>((*document)["breed"]);
                                               CHECK(breed == "french bulldog");
                                           });

        dog_collection.find_one_and_update(dog_document2, dog_document, find_and_modify_options,
                                           [&](Optional<bson::BsonDocument> document, Optional<AppError> error) {
                                               REQUIRE_FALSE(error);
                                               auto breed = static_cast<std::string>((*document)["breed"]);
                                               CHECK(breed == "king charles");
                                           });

        dog_collection.find_one_and_update_bson(dog_document, dog_document2, find_and_modify_options,
                                                [&](Optional<bson::Bson> bson, Optional<AppError> error) {
                                                    REQUIRE_FALSE(error);
                                                    auto breed = static_cast<std::string>(
                                                        static_cast<bson::BsonDocument>(*bson)["breed"]);
                                                    CHECK(breed == "french bulldog");
                                                });

        dog_collection.find_one_and_update_bson(dog_document2, dog_document, find_and_modify_options,
                                                [&](Optional<bson::Bson> bson, Optional<AppError> error) {
                                                    REQUIRE_FALSE(error);
                                                    auto breed = static_cast<std::string>(
                                                        static_cast<bson::BsonDocument>(*bson)["breed"]);
                                                    CHECK(breed == "king charles");
                                                });

        dog_collection.find_one_and_update({{"name", "invalid name"}}, {{"name", "some name"}},
                                           [&](Optional<bson::BsonDocument> document, Optional<AppError> error) {
                                               REQUIRE_FALSE(error);
                                               CHECK(!document);
                                               processed = true;
                                           });
        CHECK(processed);
        processed = false;

        dog_collection.find_one_and_update({{"name", "invalid name"}}, {{}}, find_and_modify_options,
                                           [&](Optional<bson::BsonDocument> document, Optional<AppError> error) {
                                               REQUIRE(error);
                                               CHECK(error->reason() == "insert not permitted");
                                               CHECK(!document);
                                               processed = true;
                                           });
        CHECK(processed);
    }

    SECTION("update") {
        bool processed = false;
        ObjectId dog_object_id;

        dog_collection.update_one(dog_document, dog_document2, true,
                                  [&](MongoCollection::UpdateResult result, Optional<AppError> error) {
                                      REQUIRE_FALSE(error);
                                      CHECK((*result.upserted_id).to_string() != "");
                                  });

        dog_collection.update_one(dog_document2, dog_document,
                                  [&](MongoCollection::UpdateResult result, Optional<AppError> error) {
                                      REQUIRE_FALSE(error);
                                      CHECK(!result.upserted_id);
                                  });

        cat_collection.update_one({}, cat_document, true,
                                  [&](MongoCollection::UpdateResult result, Optional<AppError> error) {
                                      REQUIRE_FALSE(error);
                                      CHECK(result.upserted_id->type() == bson::Bson::Type::String);
                                      CHECK(result.upserted_id == cat_id_string);
                                  });

        dog_collection.delete_many({}, [&](uint64_t, Optional<AppError> error) {
            REQUIRE_FALSE(error);
        });

        cat_collection.delete_many({}, [&](uint64_t, Optional<AppError> error) {
            REQUIRE_FALSE(error);
        });

        dog_collection.update_one_bson(dog_document, dog_document2, true,
                                       [&](Optional<bson::Bson> bson, Optional<AppError> error) {
                                           REQUIRE_FALSE(error);
                                           auto upserted_id = static_cast<bson::BsonDocument>(*bson)["upsertedId"];

                                           REQUIRE(upserted_id.type() == bson::Bson::Type::ObjectId);
                                       });

        dog_collection.update_one_bson(dog_document2, dog_document, true,
                                       [&](Optional<bson::Bson> bson, Optional<AppError> error) {
                                           REQUIRE_FALSE(error);
                                           auto document = static_cast<bson::BsonDocument>(*bson);
                                           auto foundUpsertedId = document.find("upsertedId");
                                           REQUIRE(!foundUpsertedId);
                                       });

        cat_collection.update_one_bson({}, cat_document, true,
                                       [&](Optional<bson::Bson> bson, Optional<AppError> error) {
                                           REQUIRE_FALSE(error);
                                           auto upserted_id = static_cast<bson::BsonDocument>(*bson)["upsertedId"];
                                           REQUIRE(upserted_id.type() == bson::Bson::Type::String);
                                           REQUIRE(upserted_id == cat_id_string);
                                       });

        person_document["dogs"] = bson::BsonArray();
        bson::BsonDocument person_document_copy = bson::BsonDocument(person_document);
        person_document_copy["dogs"] = bson::BsonArray({dog_object_id});
        person_collection.update_one(person_document, person_document, true,
                                     [&](MongoCollection::UpdateResult, Optional<AppError> error) {
                                         REQUIRE_FALSE(error);
                                         processed = true;
                                     });

        CHECK(processed);
    }

    SECTION("update many") {
        bool processed = false;

        dog_collection.insert_one(dog_document, [&](Optional<bson::Bson> object_id, Optional<AppError> error) {
            REQUIRE_FALSE(error);
            CHECK((*object_id).to_string() != "");
        });

        dog_collection.update_many(dog_document2, dog_document, true,
                                   [&](MongoCollection::UpdateResult result, Optional<AppError> error) {
                                       REQUIRE_FALSE(error);
                                       CHECK((*result.upserted_id).to_string() != "");
                                   });

        dog_collection.update_many(dog_document2, dog_document,
                                   [&](MongoCollection::UpdateResult result, Optional<AppError> error) {
                                       REQUIRE_FALSE(error);
                                       CHECK(!result.upserted_id);
                                       processed = true;
                                   });

        CHECK(processed);
    }

    SECTION("find and replace") {
        bool processed = false;
        ObjectId dog_object_id;
        ObjectId person_object_id;

        MongoCollection::FindOneAndModifyOptions find_and_modify_options{
            Optional<bson::BsonDocument>({{"name", "fido"}}), // project
            Optional<bson::BsonDocument>({{"name", 1}}),      // sort,
            true,                                             // upsert
            true                                              // return new doc
        };

        dog_collection.find_one_and_replace(dog_document, dog_document2,
                                            [&](Optional<bson::BsonDocument> document, Optional<AppError> error) {
                                                REQUIRE_FALSE(error);
                                                CHECK(!document);
                                            });

        dog_collection.insert_one(dog_document, [&](Optional<bson::Bson> object_id, Optional<AppError> error) {
            REQUIRE_FALSE(error);
            CHECK((*object_id).to_string() != "");
            dog_object_id = static_cast<ObjectId>(*object_id);
        });

        dog_collection.find_one_and_replace(dog_document, dog_document2,
                                            [&](Optional<bson::BsonDocument> document, Optional<AppError> error) {
                                                REQUIRE_FALSE(error);
                                                auto name = static_cast<std::string>((*document)["name"]);
                                                CHECK(name == "fido");
                                            });

        dog_collection.find_one_and_replace(dog_document2, dog_document, find_and_modify_options,
                                            [&](Optional<bson::BsonDocument> document, Optional<AppError> error) {
                                                REQUIRE_FALSE(error);
                                                auto name = static_cast<std::string>((*document)["name"]);
                                                CHECK(static_cast<std::string>(name) == "fido");
                                            });

        person_document["dogs"] = bson::BsonArray({dog_object_id});
        person_document2["dogs"] = bson::BsonArray({dog_object_id});
        person_collection.insert_one(person_document, [&](Optional<bson::Bson> object_id, Optional<AppError> error) {
            REQUIRE_FALSE(error);
            CHECK((*object_id).to_string() != "");
            person_object_id = static_cast<ObjectId>(*object_id);
        });

        MongoCollection::FindOneAndModifyOptions person_find_and_modify_options{
            Optional<bson::BsonDocument>({{"firstName", 1}}), // project
            Optional<bson::BsonDocument>({{"firstName", 1}}), // sort,
            false,                                            // upsert
            true                                              // return new doc
        };

        person_collection.find_one_and_replace(person_document, person_document2,
                                               [&](Optional<bson::BsonDocument> document, Optional<AppError> error) {
                                                   REQUIRE_FALSE(error);
                                                   auto name = static_cast<std::string>((*document)["firstName"]);
                                                   // Should return the old document
                                                   CHECK(name == "John");
                                                   processed = true;
                                               });

        person_collection.find_one_and_replace(person_document2, person_document, person_find_and_modify_options,
                                               [&](Optional<bson::BsonDocument> document, Optional<AppError> error) {
                                                   REQUIRE_FALSE(error);
                                                   auto name = static_cast<std::string>((*document)["firstName"]);
                                                   // Should return new document, Bob -> John
                                                   CHECK(name == "John");
                                               });

        person_collection.find_one_and_replace({{"invalid", "item"}}, {{}},
                                               [&](Optional<bson::BsonDocument> document, Optional<AppError> error) {
                                                   // If a document is not found then null will be returned for the
                                                   // document and no error will be returned
                                                   REQUIRE_FALSE(error);
                                                   CHECK(!document);
                                               });

        person_collection.find_one_and_replace({{"invalid", "item"}}, {{}}, person_find_and_modify_options,
                                               [&](Optional<bson::BsonDocument> document, Optional<AppError> error) {
                                                   REQUIRE_FALSE(error);
                                                   CHECK(!document);
                                                   processed = true;
                                               });

        CHECK(processed);
    }

    SECTION("delete") {

        bool processed = false;

        bson::BsonArray documents;
        documents.push_back(dog_document);
        documents.push_back(dog_document);
        documents.push_back(dog_document);

        dog_collection.insert_many(documents, [&](bson::BsonArray inserted_docs, Optional<AppError> error) {
            REQUIRE_FALSE(error);
            CHECK(inserted_docs.size() == 3);
        });

        MongoCollection::FindOneAndModifyOptions find_and_modify_options{
            Optional<bson::BsonDocument>({{"name", "fido"}}), // project
            Optional<bson::BsonDocument>({{"name", 1}}),      // sort,
            true,                                             // upsert
            true                                              // return new doc
        };

        dog_collection.delete_one(dog_document, [&](uint64_t deleted_count, Optional<AppError> error) {
            REQUIRE_FALSE(error);
            CHECK(deleted_count >= 1);
        });

        dog_collection.delete_many(dog_document, [&](uint64_t deleted_count, Optional<AppError> error) {
            REQUIRE_FALSE(error);
            CHECK(deleted_count >= 1);
            processed = true;
        });

        person_collection.delete_many_bson(person_document, [&](Optional<bson::Bson> bson, Optional<AppError> error) {
            REQUIRE_FALSE(error);
            CHECK(static_cast<int32_t>(static_cast<bson::BsonDocument>(*bson)["deletedCount"]) >= 1);
            processed = true;
        });

        CHECK(processed);
    }
}

// MARK: - Push Notifications Tests

TEST_CASE("app: push notifications", "[sync][app][notifications][baas]") {
    TestAppSession session;
    auto app = session.app();
    std::shared_ptr<SyncUser> sync_user = app->current_user();

    SECTION("register") {
        bool processed;

        app->push_notification_client("gcm").register_device("hello", sync_user, [&](Optional<AppError> error) {
            REQUIRE_FALSE(error);
            processed = true;
        });

        CHECK(processed);
    }
    /*
        // FIXME: It seems this test fails when the two register_device calls are invoked too quickly,
        // The error returned will be 'Device not found' on the second register_device call.
        SECTION("register twice") {
            // registering the same device twice should not result in an error
            bool processed;

            app->push_notification_client("gcm").register_device("hello",
                                                                 sync_user,
                                                                 [&](Optional<AppError> error) {
                REQUIRE_FALSE(error);
            });

            app->push_notification_client("gcm").register_device("hello",
                                                                 sync_user,
                                                                 [&](Optional<AppError> error) {
                REQUIRE_FALSE(error);
                processed = true;
            });

            CHECK(processed);
        }
    */
    SECTION("deregister") {
        bool processed;

        app->push_notification_client("gcm").deregister_device(sync_user, [&](Optional<AppError> error) {
            REQUIRE_FALSE(error);
            processed = true;
        });
        CHECK(processed);
    }

    SECTION("register with unavailable service") {
        bool processed;

        app->push_notification_client("gcm_blah").register_device("hello", sync_user, [&](Optional<AppError> error) {
            REQUIRE(error);
            CHECK(error->reason() == "service not found: 'gcm_blah'");
            processed = true;
        });
        CHECK(processed);
    }

    SECTION("register with logged out user") {
        bool processed;

        app->log_out([=](Optional<AppError> error) {
            REQUIRE_FALSE(error);
        });

        app->push_notification_client("gcm").register_device("hello", sync_user, [&](Optional<AppError> error) {
            REQUIRE(error);
            processed = true;
        });

        app->push_notification_client("gcm").register_device("hello", nullptr, [&](Optional<AppError> error) {
            REQUIRE(error);
            processed = true;
        });

        CHECK(processed);
    }
}

// MARK: - Token refresh

TEST_CASE("app: token refresh", "[sync][app][token][baas]") {
    TestAppSession session;
    auto app = session.app();
    std::shared_ptr<SyncUser> sync_user = app->current_user();
    sync_user->update_access_token(ENCODE_FAKE_JWT("fake_access_token"));

    auto remote_client = app->current_user()->mongo_client("BackingDB");
    auto app_session = get_runtime_app_session();
    auto db = remote_client.db(app_session.config.mongo_dbname);
    auto dog_collection = db["Dog"];
    bson::BsonDocument dog_document{{"name", "fido"}, {"breed", "king charles"}};

    SECTION("access token should refresh") {
        /*
         Expected sequence of events:
         - `find_one` tries to hit the server with a bad access token
         - Server returns an error because of the bad token, error should be something like:
            {\"error\":\"json: cannot unmarshal array into Go value of type map[string]interface
         {}\",\"link\":\"http://localhost:9090/groups/5f84167e776aa0f9dc27081a/apps/5f841686776aa0f9dc270876/logs?co_id=5f844c8c776aa0f9dc273db6\"}
            http_status_code = 401
            custom_status_code = 0
         - App::handle_auth_failure is then called and an attempt to refresh the access token will be peformed.
         - If the token refresh was successful, the original request will retry and we should expect no error in the
         callback of `find_one`
         */
        dog_collection.find_one(dog_document, [&](Optional<bson::BsonDocument>, Optional<AppError> error) {
            REQUIRE_FALSE(error);
        });
    }
}

// MARK: - Sync Tests

TEST_CASE("app: mixed lists with object links", "[sync][pbs][app][links][baas]") {
    const std::string valid_pk_name = "_id";

    Schema schema{
        {"TopLevel",
         {
             {valid_pk_name, PropertyType::ObjectId, Property::IsPrimary{true}},
             {"mixed_array", PropertyType::Mixed | PropertyType::Array | PropertyType::Nullable},
         }},
        {"Target",
         {
             {valid_pk_name, PropertyType::ObjectId, Property::IsPrimary{true}},
             {"value", PropertyType::Int},
         }},
    };

    auto server_app_config = minimal_app_config("set_new_embedded_object", schema);
    auto app_session = create_app(server_app_config);
    auto partition = random_string(100);

    auto obj_id = ObjectId::gen();
    auto target_id = ObjectId::gen();
    auto mixed_list_values = AnyVector{
        Mixed{int64_t(1234)},
        Mixed{},
        Mixed{target_id},
    };
    {
        TestAppSession test_session(app_session, nullptr, DeleteApp{false});
        SyncTestFile config(test_session.app()->current_user(), partition, schema);
        auto realm = Realm::get_shared_realm(config);

        CppContext c(realm);
        realm->begin_transaction();
        auto target_obj = Object::create(
            c, realm, "Target", std::any(AnyDict{{valid_pk_name, target_id}, {"value", static_cast<int64_t>(1234)}}));
        mixed_list_values.push_back(Mixed(target_obj.get_obj().get_link()));

        Object::create(c, realm, "TopLevel",
                       std::any(AnyDict{
                           {valid_pk_name, obj_id},
                           {"mixed_array", mixed_list_values},
                       }),
                       CreatePolicy::ForceCreate);
        realm->commit_transaction();
        CHECK(!wait_for_upload(*realm));
    }

    {
        TestAppSession test_session(app_session);
        SyncTestFile config(test_session.app(), partition, schema);
        auto realm = Realm::get_shared_realm(config);

        CHECK(!wait_for_download(*realm));
        CppContext c(realm);
        auto obj = Object::get_for_primary_key(c, realm, "TopLevel", std::any{obj_id});
        auto list = util::any_cast<List&&>(obj.get_property_value<std::any>(c, "mixed_array"));
        for (size_t idx = 0; idx < list.size(); ++idx) {
            Mixed mixed = list.get_any(idx);
            if (idx == 3) {
                CHECK(mixed.is_type(type_TypedLink));
                auto link = mixed.get<ObjLink>();
                auto link_table = realm->read_group().get_table(link.get_table_key());
                CHECK(link_table->get_name() == "class_Target");
                auto link_obj = link_table->get_object(link.get_obj_key());
                CHECK(link_obj.get_primary_key() == target_id);
            }
            else {
                CHECK(mixed == util::any_cast<Mixed>(mixed_list_values[idx]));
            }
        }
    }
}

TEST_CASE("app: roundtrip values", "[sync][pbs][app][baas]") {
    const std::string valid_pk_name = "_id";

    Schema schema{
        {"TopLevel",
         {
             {valid_pk_name, PropertyType::ObjectId, Property::IsPrimary{true}},
             {"decimal", PropertyType::Decimal | PropertyType::Nullable},
         }},
    };

    auto server_app_config = minimal_app_config("roundtrip_values", schema);
    auto app_session = create_app(server_app_config);
    auto partition = random_string(100);

    Decimal128 large_significand = Decimal128(70) / Decimal128(1.09);
    auto obj_id = ObjectId::gen();
    {
        TestAppSession test_session(app_session, nullptr, DeleteApp{false});
        SyncTestFile config(test_session.app(), partition, schema);
        auto realm = Realm::get_shared_realm(config);

        CppContext c(realm);
        realm->begin_transaction();
        Object::create(c, realm, "TopLevel",
                       util::Any(AnyDict{
                           {valid_pk_name, obj_id},
                           {"decimal", large_significand},
                       }),
                       CreatePolicy::ForceCreate);
        realm->commit_transaction();
        CHECK(!wait_for_upload(*realm, std::chrono::seconds(600)));
    }

    {
        TestAppSession test_session(app_session);
        SyncTestFile config(test_session.app(), partition, schema);
        auto realm = Realm::get_shared_realm(config);

        CHECK(!wait_for_download(*realm));
        CppContext c(realm);
        auto obj = Object::get_for_primary_key(c, realm, "TopLevel", util::Any{obj_id});
        auto val = obj.get_column_value<Decimal128>("decimal");
        CHECK(val == large_significand);
    }
}

TEST_CASE("app: upgrade from local to synced realm", "[sync][pbs][app][upgrade][baas]") {
    const std::string valid_pk_name = "_id";

    Schema schema{
        {"origin",
         {{valid_pk_name, PropertyType::Int, Property::IsPrimary{true}},
          {"link", PropertyType::Object | PropertyType::Nullable, "target"},
          {"embedded_link", PropertyType::Object | PropertyType::Nullable, "embedded"}}},
        {"target",
         {{valid_pk_name, PropertyType::String, Property::IsPrimary{true}},
          {"value", PropertyType::Int},
          {"name", PropertyType::String}}},
        {"other_origin",
         {{valid_pk_name, PropertyType::ObjectId, Property::IsPrimary{true}},
          {"array", PropertyType::Array | PropertyType::Object, "other_target"}}},
        {"other_target",
         {{valid_pk_name, PropertyType::UUID, Property::IsPrimary{true}}, {"value", PropertyType::Int}}},
        {"embedded", ObjectSchema::ObjectType::Embedded, {{"name", PropertyType::String | PropertyType::Nullable}}},
    };

    /*             Create local realm             */
    TestFile local_config;
    local_config.schema = schema;
    auto local_realm = Realm::get_shared_realm(local_config);
    {
        auto origin = local_realm->read_group().get_table("class_origin");
        auto target = local_realm->read_group().get_table("class_target");
        auto other_origin = local_realm->read_group().get_table("class_other_origin");
        auto other_target = local_realm->read_group().get_table("class_other_target");

        local_realm->begin_transaction();
        auto o = target->create_object_with_primary_key("Foo").set("name", "Egon");
        // 'embedded_link' property is null.
        origin->create_object_with_primary_key(47).set("link", o.get_key());
        // 'embedded_link' property is not null.
        auto obj = origin->create_object_with_primary_key(42);
        auto col_key = origin->get_column_key("embedded_link");
        obj.create_and_set_linked_object(col_key);
        other_target->create_object_with_primary_key(UUID("3b241101-e2bb-4255-8caf-4136c566a961"));
        other_origin->create_object_with_primary_key(ObjectId::gen());
        local_realm->commit_transaction();
    }

    /* Create a synced realm and upload some data */
    auto server_app_config = minimal_app_config("upgrade_from_local", schema);
    TestAppSession test_session(create_app(server_app_config));
    auto partition = random_string(100);
    auto user1 = test_session.app()->current_user();
    SyncTestFile config1(user1, partition, schema);

    auto r1 = Realm::get_shared_realm(config1);

    auto origin = r1->read_group().get_table("class_origin");
    auto target = r1->read_group().get_table("class_target");
    auto other_origin = r1->read_group().get_table("class_other_origin");
    auto other_target = r1->read_group().get_table("class_other_target");

    r1->begin_transaction();
    auto o = target->create_object_with_primary_key("Baa").set("name", "Børge");
    origin->create_object_with_primary_key(47).set("link", o.get_key());
    other_target->create_object_with_primary_key(UUID("01234567-89ab-cdef-edcb-a98765432101"));
    other_origin->create_object_with_primary_key(ObjectId::gen());
    r1->commit_transaction();
    CHECK(!wait_for_upload(*r1));

    /* Copy local realm data over in a synced one*/
    create_user_and_log_in(test_session.app());
    auto user2 = test_session.app()->current_user();
    REQUIRE(user1 != user2);

    SyncTestFile config2(user1, partition, schema);

    SharedRealm r2;
    SECTION("Copy before connecting to server") {
        local_realm->convert(config2);
        r2 = Realm::get_shared_realm(config2);
    }

    SECTION("Open synced realm first") {
        r2 = Realm::get_shared_realm(config2);
        CHECK(!wait_for_download(*r2));
        local_realm->convert(config2);
        CHECK(!wait_for_upload(*r2));
    }

    CHECK(!wait_for_download(*r2));
    advance_and_notify(*r2);
    Group& g = r2->read_group();
    // g.to_json(std::cout);
    REQUIRE(g.get_table("class_origin")->size() == 2);
    REQUIRE(g.get_table("class_target")->size() == 2);
    REQUIRE(g.get_table("class_other_origin")->size() == 2);
    REQUIRE(g.get_table("class_other_target")->size() == 2);

    CHECK(!wait_for_upload(*r2));
    CHECK(!wait_for_download(*r1));
    advance_and_notify(*r1);
    // r1->read_group().to_json(std::cout);
}

TEST_CASE("app: set new embedded object", "[sync][pbs][app][baas]") {
    const std::string valid_pk_name = "_id";

    Schema schema{
        {"TopLevel",
         {
             {valid_pk_name, PropertyType::ObjectId, Property::IsPrimary{true}},
             {"array_of_objs", PropertyType::Object | PropertyType::Array, "TopLevel_array_of_objs"},
             {"embedded_obj", PropertyType::Object | PropertyType::Nullable, "TopLevel_embedded_obj"},
             {"embedded_dict", PropertyType::Object | PropertyType::Dictionary | PropertyType::Nullable,
              "TopLevel_embedded_dict"},
         }},
        {"TopLevel_array_of_objs",
         ObjectSchema::ObjectType::Embedded,
         {
             {"array", PropertyType::Int | PropertyType::Array},
         }},
        {"TopLevel_embedded_obj",
         ObjectSchema::ObjectType::Embedded,
         {
             {"array", PropertyType::Int | PropertyType::Array},
         }},
        {"TopLevel_embedded_dict",
         ObjectSchema::ObjectType::Embedded,
         {
             {"array", PropertyType::Int | PropertyType::Array},
         }},
    };

    auto server_app_config = minimal_app_config("set_new_embedded_object", schema);
    TestAppSession test_session(create_app(server_app_config));
    auto partition = random_string(100);

    auto array_of_objs_id = ObjectId::gen();
    auto embedded_obj_id = ObjectId::gen();
    auto dict_obj_id = ObjectId::gen();

    {
        SyncTestFile config(test_session.app(), partition, schema);
        auto realm = Realm::get_shared_realm(config);

        CppContext c(realm);
        realm->begin_transaction();
        auto array_of_objs =
            Object::create(c, realm, "TopLevel",
                           std::any(AnyDict{
                               {valid_pk_name, array_of_objs_id},
                               {"array_of_objs", AnyVector{AnyDict{{"array", AnyVector{INT64_C(1), INT64_C(2)}}}}},
                           }),
                           CreatePolicy::ForceCreate);

        auto embedded_obj =
            Object::create(c, realm, "TopLevel",
                           std::any(AnyDict{
                               {valid_pk_name, embedded_obj_id},
                               {"embedded_obj", AnyDict{{"array", AnyVector{INT64_C(1), INT64_C(2)}}}},
                           }),
                           CreatePolicy::ForceCreate);

        auto dict_obj = Object::create(
            c, realm, "TopLevel",
            std::any(AnyDict{
                {valid_pk_name, dict_obj_id},
                {"embedded_dict", AnyDict{{"foo", AnyDict{{"array", AnyVector{INT64_C(1), INT64_C(2)}}}}}},
            }),
            CreatePolicy::ForceCreate);

        realm->commit_transaction();
        {
            realm->begin_transaction();
            embedded_obj.set_property_value(c, "embedded_obj",
                                            std::any(AnyDict{{
                                                "array",
                                                AnyVector{INT64_C(3), INT64_C(4)},
                                            }}),
                                            CreatePolicy::UpdateAll);
            realm->commit_transaction();
        }

        {
            realm->begin_transaction();
            List array(array_of_objs, array_of_objs.get_object_schema().property_for_name("array_of_objs"));
            CppContext c2(realm, &array.get_object_schema());
            array.set(c2, 0, std::any{AnyDict{{"array", AnyVector{INT64_C(5), INT64_C(6)}}}});
            realm->commit_transaction();
        }

        {
            realm->begin_transaction();
            object_store::Dictionary dict(dict_obj, dict_obj.get_object_schema().property_for_name("embedded_dict"));
            CppContext c2(realm, &dict.get_object_schema());
            dict.insert(c2, "foo", std::any{AnyDict{{"array", AnyVector{INT64_C(7), INT64_C(8)}}}});
            realm->commit_transaction();
        }
        CHECK(!wait_for_upload(*realm));
    }

    {
        SyncTestFile config(test_session.app(), partition, schema);
        auto realm = Realm::get_shared_realm(config);

        CHECK(!wait_for_download(*realm));
        CppContext c(realm);
        {
            auto obj = Object::get_for_primary_key(c, realm, "TopLevel", std::any{embedded_obj_id});
            auto embedded_obj = util::any_cast<Object&&>(obj.get_property_value<std::any>(c, "embedded_obj"));
            auto array_list = util::any_cast<List&&>(embedded_obj.get_property_value<std::any>(c, "array"));
            CHECK(array_list.size() == 2);
            CHECK(array_list.get<int64_t>(0) == int64_t(3));
            CHECK(array_list.get<int64_t>(1) == int64_t(4));
        }

        {
            auto obj = Object::get_for_primary_key(c, realm, "TopLevel", std::any{array_of_objs_id});
            auto embedded_list = util::any_cast<List&&>(obj.get_property_value<std::any>(c, "array_of_objs"));
            CppContext c2(realm, &embedded_list.get_object_schema());
            auto embedded_array_obj = util::any_cast<Object&&>(embedded_list.get(c2, 0));
            auto array_list = util::any_cast<List&&>(embedded_array_obj.get_property_value<std::any>(c2, "array"));
            CHECK(array_list.size() == 2);
            CHECK(array_list.get<int64_t>(0) == int64_t(5));
            CHECK(array_list.get<int64_t>(1) == int64_t(6));
        }

        {
            auto obj = Object::get_for_primary_key(c, realm, "TopLevel", std::any{dict_obj_id});
            object_store::Dictionary dict(obj, obj.get_object_schema().property_for_name("embedded_dict"));
            CppContext c2(realm, &dict.get_object_schema());
            auto embedded_obj = util::any_cast<Object&&>(dict.get(c2, "foo"));
            auto array_list = util::any_cast<List&&>(embedded_obj.get_property_value<std::any>(c2, "array"));
            CHECK(array_list.size() == 2);
            CHECK(array_list.get<int64_t>(0) == int64_t(7));
            CHECK(array_list.get<int64_t>(1) == int64_t(8));
        }
    }
}

TEST_CASE("app: make distributable client file", "[sync][pbs][app][baas]") {
    TestAppSession session;
    auto app = session.app();

    auto schema = get_default_schema();
    SyncTestFile original_config(app, bson::Bson("foo"), schema);
    create_user_and_log_in(app);
    SyncTestFile target_config(app, bson::Bson("foo"), schema);

    // Create realm file without client file id
    {
        auto realm = Realm::get_shared_realm(original_config);

        // Write some data
        realm->begin_transaction();
        CppContext c;
        Object::create(c, realm, "Person",
                       std::any(realm::AnyDict{{"_id", std::any(ObjectId::gen())},
                                               {"age", INT64_C(64)},
                                               {"firstName", std::string("Paul")},
                                               {"lastName", std::string("McCartney")}}));
        realm->commit_transaction();
        wait_for_upload(*realm);
        wait_for_download(*realm);

        realm->convert(target_config);

        // Write some additional data
        realm->begin_transaction();
        Object::create(c, realm, "Dog",
                       std::any(realm::AnyDict{{"_id", std::any(ObjectId::gen())},
                                               {"breed", std::string("stabyhoun")},
                                               {"name", std::string("albert")},
                                               {"realm_id", std::string("foo")}}));
        realm->commit_transaction();
        wait_for_upload(*realm);
    }
    // Starting a new session based on the copy
    {
        auto realm = Realm::get_shared_realm(target_config);
        REQUIRE(realm->read_group().get_table("class_Person")->size() == 1);
        REQUIRE(realm->read_group().get_table("class_Dog")->size() == 0);

        // Should be able to download the object created in the source Realm
        // after writing the copy
        wait_for_download(*realm);
        realm->refresh();
        REQUIRE(realm->read_group().get_table("class_Person")->size() == 1);
        REQUIRE(realm->read_group().get_table("class_Dog")->size() == 1);

        // Check that we can continue committing to this realm
        realm->begin_transaction();
        CppContext c;
        Object::create(c, realm, "Dog",
                       std::any(realm::AnyDict{{"_id", std::any(ObjectId::gen())},
                                               {"breed", std::string("bulldog")},
                                               {"name", std::string("fido")},
                                               {"realm_id", std::string("foo")}}));
        realm->commit_transaction();
        wait_for_upload(*realm);
    }
    // Original Realm should be able to read the object which was written to the copy
    {
        auto realm = Realm::get_shared_realm(original_config);
        REQUIRE(realm->read_group().get_table("class_Person")->size() == 1);
        REQUIRE(realm->read_group().get_table("class_Dog")->size() == 1);

        wait_for_download(*realm);
        realm->refresh();
        REQUIRE(realm->read_group().get_table("class_Person")->size() == 1);
        REQUIRE(realm->read_group().get_table("class_Dog")->size() == 2);
    }
}

struct HookedSocketProvider : public sync::websocket::DefaultSocketProvider {
    HookedSocketProvider(const std::shared_ptr<util::Logger>& logger, const std::string user_agent,
                         AutoStart auto_start = AutoStart{true})
        : DefaultSocketProvider(logger, user_agent, nullptr, auto_start)
    {
    }

    std::unique_ptr<sync::WebSocketInterface> connect(std::unique_ptr<sync::WebSocketObserver> observer,
                                                      sync::WebSocketEndpoint&& endpoint) override
    {
        using WebSocketError = sync::websocket::WebSocketError;

        int status_code = 101;
        bool was_clean = true;
        WebSocketError ws_error = WebSocketError::websocket_ok;
        std::string body;

        sync::WebSocketEndpoint ep{std::move(endpoint)};
        if (endpoint_verify_func) {
            endpoint_verify_func(ep);
        }

        if (force_failure_func && force_failure_func(was_clean, ws_error, body)) {
            observer->websocket_error_handler();
            observer->websocket_closed_handler(was_clean, ws_error, body);
            return nullptr;
        }

        bool use_simulated_response = websocket_connect_func && websocket_connect_func(status_code, body);
        auto websocket = DefaultSocketProvider::connect(std::move(observer), std::move(ep));
        if (use_simulated_response) {
            auto default_websocket = static_cast<sync::websocket::DefaultWebSocket*>(websocket.get());
            if (default_websocket)
                default_websocket->force_handshake_response_for_testing(status_code, body);
        }
        return websocket;
    }

    std::function<void(sync::WebSocketEndpoint& endpoint)> endpoint_verify_func;
    std::function<bool(bool& was_clean, sync::websocket::WebSocketError& error_code, std::string& message)>
        force_failure_func;
    std::function<bool(int&, std::string&)> websocket_connect_func;
};

TEST_CASE("app: sync integration", "[sync][pbs][app][baas]") {
    auto logger = util::Logger::get_default_logger();

    const auto schema = get_default_schema();

    auto get_dogs = [](SharedRealm r) -> Results {
        wait_for_upload(*r, std::chrono::seconds(10));
        wait_for_download(*r, std::chrono::seconds(10));
        return Results(r, r->read_group().get_table("class_Dog"));
    };

    auto create_one_dog = [](SharedRealm r) {
        r->begin_transaction();
        CppContext c;
        Object::create(c, r, "Dog",
                       std::any(AnyDict{{"_id", std::any(ObjectId::gen())},
                                        {"breed", std::string("bulldog")},
                                        {"name", std::string("fido")}}),
                       CreatePolicy::ForceCreate);
        r->commit_transaction();
    };

    TestAppSession session;
    auto app = session.app();
    const auto partition = random_string(100);

    // MARK: Add Objects -
    SECTION("Add Objects") {
        {
            SyncTestFile config(app, partition, schema);
            auto r = Realm::get_shared_realm(config);

            REQUIRE(get_dogs(r).size() == 0);
            create_one_dog(r);
            REQUIRE(get_dogs(r).size() == 1);
        }

        {
            create_user_and_log_in(app);
            SyncTestFile config(app, partition, schema);
            auto r = Realm::get_shared_realm(config);
            Results dogs = get_dogs(r);
            REQUIRE(dogs.size() == 1);
            REQUIRE(dogs.get(0).get<String>("breed") == "bulldog");
            REQUIRE(dogs.get(0).get<String>("name") == "fido");
        }
    }

    SECTION("MemOnly durability") {
        {
            SyncTestFile config(app, partition, schema);
            config.in_memory = true;
            config.encryption_key = std::vector<char>();

            REQUIRE(config.options().durability == DBOptions::Durability::MemOnly);
            auto r = Realm::get_shared_realm(config);

            REQUIRE(get_dogs(r).size() == 0);
            create_one_dog(r);
            REQUIRE(get_dogs(r).size() == 1);
        }

        {
            create_user_and_log_in(app);
            SyncTestFile config(app, partition, schema);
            config.in_memory = true;
            config.encryption_key = std::vector<char>();
            auto r = Realm::get_shared_realm(config);
            Results dogs = get_dogs(r);
            REQUIRE(dogs.size() == 1);
            REQUIRE(dogs.get(0).get<String>("breed") == "bulldog");
            REQUIRE(dogs.get(0).get<String>("name") == "fido");
        }
    }

    // MARK: Expired Session Refresh -
    SECTION("Invalid Access Token is Refreshed") {
        {
            SyncTestFile config(app, partition, schema);
            auto r = Realm::get_shared_realm(config);
            REQUIRE(get_dogs(r).size() == 0);
            create_one_dog(r);
            REQUIRE(get_dogs(r).size() == 1);
        }

        {
            create_user_and_log_in(app);
            auto user = app->current_user();
            // set a bad access token. this will trigger a refresh when the sync session opens
            user->update_access_token(encode_fake_jwt("fake_access_token"));

            SyncTestFile config(app, partition, schema);
            auto r = Realm::get_shared_realm(config);
            Results dogs = get_dogs(r);
            REQUIRE(dogs.size() == 1);
            REQUIRE(dogs.get(0).get<String>("breed") == "bulldog");
            REQUIRE(dogs.get(0).get<String>("name") == "fido");
        }
    }

    class HookedTransport : public SynchronousTestTransport {
    public:
        void send_request_to_server(const Request& request,
                                    util::UniqueFunction<void(const Response&)>&& completion) override
        {
            if (request_hook) {
                if (auto simulated_response = request_hook(request)) {
                    return completion(*simulated_response);
                }
            }
            SynchronousTestTransport::send_request_to_server(request, [&](const Response& response) mutable {
                if (response_hook) {
                    response_hook(request, response);
                }
                completion(response);
            });
        }
        // Optional handler for the request and response before it is returned to completion
        std::function<void(const Request&, const Response&)> response_hook;
        // Optional handler for the request before it is sent to the server
        std::function<std::optional<Response>(const Request&)> request_hook;
    };

    struct HookedSocketProvider : public sync::websocket::DefaultSocketProvider {
        HookedSocketProvider(const std::shared_ptr<util::Logger>& logger, const std::string user_agent,
                             AutoStart auto_start = AutoStart{true})
            : DefaultSocketProvider(logger, user_agent, nullptr, auto_start)
        {
        }

        std::unique_ptr<sync::WebSocketInterface> connect(std::unique_ptr<sync::WebSocketObserver> observer,
                                                          sync::WebSocketEndpoint&& endpoint) override
        {
            auto simulated_response = websocket_connect_simulated_response_func
                                          ? websocket_connect_simulated_response_func()
                                          : std::nullopt;

            if (websocket_endpoint_resolver) {
                endpoint = websocket_endpoint_resolver(std::move(endpoint));
            }
            std::unique_ptr<sync::WebSocketInterface> websocket =
                DefaultSocketProvider::connect(std::move(observer), std::move(endpoint));
            if (simulated_response) {
                auto default_websocket = static_cast<sync::websocket::DefaultWebSocket*>(websocket.get());
                if (default_websocket)
                    default_websocket->force_handshake_response_for_testing(*simulated_response, "");
            }
            return websocket;
        }

        std::function<std::optional<int>()> websocket_connect_simulated_response_func;
        std::function<sync::WebSocketEndpoint(sync::WebSocketEndpoint&&)> websocket_endpoint_resolver;
    };

    {
        std::unique_ptr<realm::AppSession> app_session;
        auto redir_transport = std::make_shared<HookedTransport>();
        AutoVerifiedEmailCredentials creds;

        auto app_config = get_config(redir_transport, session.app_session());
        set_app_config_defaults(app_config, redir_transport);

        SyncClientConfig sc_config;
        sc_config.base_file_path = util::make_temp_dir();
        sc_config.metadata_mode = realm::SyncManager::MetadataMode::NoEncryption;

        // initialize app and sync client
        auto redir_app = app::App::get_app(app::App::CacheMode::Disabled, app_config, sc_config);

        SECTION("Test invalid redirect response") {
            int request_count = 0;
            redir_transport->request_hook = [&](const Request& request) -> std::optional<Response> {
                if (request_count == 0) {
                    logger->trace("request.url (%1): %2", request_count, request.url);
                    ++request_count;
                    return Response{301, 0, {{"Content-Type", "application/json"}}, "Some body data"};
                }
                else if (request_count == 1) {
                    logger->trace("request.url (%1): %2", request_count, request.url);
                    return Response{
                        301, 0, {{"Location", ""}, {"Content-Type", "application/json"}}, "Some body data"};
                }

                return std::nullopt;
            };

            // This will fail due to no Location header
            redir_app->provider_client<app::App::UsernamePasswordProviderClient>().register_email(
                creds.email, creds.password, [&](util::Optional<app::AppError> error) {
                    REQUIRE(error);
                    REQUIRE(error->is_client_error());
                    REQUIRE(error->code() == ErrorCodes::ClientRedirectError);
                    REQUIRE(error->reason() == "Redirect response missing location header");
                });

            // This will fail due to empty Location header
            redir_app->provider_client<app::App::UsernamePasswordProviderClient>().register_email(
                creds.email, creds.password, [&](util::Optional<app::AppError> error) {
                    REQUIRE(error);
                    REQUIRE(error->is_client_error());
                    REQUIRE(error->code() == ErrorCodes::ClientRedirectError);
                    REQUIRE(error->reason() == "Redirect response missing location header");
                });
        }

        SECTION("Test redirect response") {
            int request_count = 0;
            // redirect URL is localhost or 127.0.0.1 depending on what the initial value is
            const std::string original_url = get_base_url();
            std::string original_host = original_url.substr(original_url.find("://") + 3);
            original_host = original_host.substr(0, original_host.find("/"));
            std::string original_ws_host = util::format("ws://%1", original_host);
            std::string redirect_scheme = "http://";
            std::string websocket_scheme = "ws://";
            const std::string redirect_host = "fakerealm.example.com:9090";
            const std::string redirect_url = "http://fakerealm.example.com:9090";
            const std::string redirect_ws = "ws://fakerealm.example.com:9090";
            redir_transport->request_hook = [&](const Request& request) -> std::optional<Response> {
                logger->trace("Received request[%1]: %2", request_count, request.url);
                if (request_count == 0) {
                    // First request should be to location
                    REQUIRE(request.url.find("/location") != std::string::npos);
                    if (request.url.find("https://") != std::string::npos) {
                        redirect_scheme = "https://";
                    }
                    logger->trace("redirect_url (%1): %2", request_count, redirect_url);
                    request_count++;
                }
                else if (request_count == 1) {
                    logger->trace("request.url (%1): %2", request_count, request.url);
                    REQUIRE(!request.redirect_count);
                    ++request_count;
                    return Response{301,
                                    0,
                                    {{"Location", "http://somehost:9090"}, {"Content-Type", "application/json"}},
                                    "Some body data"};
                }
                else if (request_count == 2) {
                    logger->trace("request.url (%1): %2", request_count, request.url);
                    REQUIRE(request.url.find("somehost:9090") != std::string::npos);
                    ++request_count;
                    return Response{
                        308, 0, {{"Location", redirect_url}, {"Content-Type", "application/json"}}, "Some body data"};
                }
                else if (request_count == 3) {
                    logger->trace("request.url (%1): %2", request_count, request.url);
                    REQUIRE(request.url.find(redirect_url) != std::string::npos);
                    ++request_count;
                    return Response{
                        301,
                        0,
                        {{"Location", redirect_scheme + original_host}, {"Content-Type", "application/json"}},
                        "Some body data"};
                }
                else if (request_count == 4) {
                    logger->trace("request.url (%1): %2", request_count, request.url);
                    REQUIRE(request.url.find(redirect_scheme + original_host) != std::string::npos);
                    // Let the init_app_metadata request go through
                    request_count++;
                }
                else if (request_count == 5) {
                    // This is the original request after the location has been updated
                    logger->trace("request.url (%1): %2", request_count, request.url);
                    // App metadata is no longer being used, query the host_url from app
                    REQUIRE(redir_app->get_host_url().find(original_host) != std::string::npos);
                    REQUIRE(request.url.find(redirect_scheme + original_host) != std::string::npos);
                    // Validate the retry count tracked in the original message
                    request_count++;
                }
                return std::nullopt;
            };

            // This will be successful after a couple of retries due to the redirect response
            redir_app->provider_client<app::App::UsernamePasswordProviderClient>().register_email(
                creds.email, creds.password, [&](util::Optional<app::AppError> error) {
                    REQUIRE(!error);
                });
        }

        SECTION("Test too many redirects") {
            int request_count = 0;
            redir_transport->request_hook = [&](const Request& request) -> std::optional<Response> {
                logger->trace("request.url (%1): %2", request_count, request.url);
                REQUIRE(request_count <= 21);
                ++request_count;
                return Response{request_count % 2 == 1 ? 308 : 301,
                                0,
                                {{"Location", "http://somehost:9090"}, {"Content-Type", "application/json"}},
                                "Some body data"};
            };

            redir_app->log_in_with_credentials(
                realm::app::AppCredentials::username_password(creds.email, creds.password),
                [&](std::shared_ptr<realm::SyncUser> user, util::Optional<app::AppError> error) {
                    REQUIRE(!user);
                    REQUIRE(error);
                    REQUIRE(error->is_client_error());
                    REQUIRE(error->code() == ErrorCodes::ClientTooManyRedirects);
                    REQUIRE(error->reason() == "number of redirections exceeded 20");
                });
        }
        SECTION("Test server in maintenance") {
            redir_transport->request_hook = [&](const Request&) -> std::optional<Response> {
                nlohmann::json maintenance_error = {{"error_code", "MaintenanceInProgress"},
                                                    {"error", "This service is currently undergoing maintenance"},
                                                    {"link", "https://link.to/server_logs"}};
                return Response{500, 0, {{"Content-Type", "application/json"}}, maintenance_error.dump()};
            };

            redir_app->log_in_with_credentials(
                realm::app::AppCredentials::username_password(creds.email, creds.password),
                [&](std::shared_ptr<realm::SyncUser> user, util::Optional<app::AppError> error) {
                    REQUIRE(!user);
                    REQUIRE(error);
                    REQUIRE(error->is_service_error());
                    REQUIRE(error->code() == ErrorCodes::MaintenanceInProgress);
                    REQUIRE(error->reason() == "This service is currently undergoing maintenance");
                    REQUIRE(error->link_to_server_logs == "https://link.to/server_logs");
                    REQUIRE(*error->additional_status_code == 500);
                });
        }
    }
    SECTION("Test app redirect with no metadata") {
        std::unique_ptr<realm::AppSession> app_session;
        auto redir_transport = std::make_shared<HookedTransport>();
        AutoVerifiedEmailCredentials creds, creds2;

        auto app_config = get_config(redir_transport, session.app_session());
        set_app_config_defaults(app_config, redir_transport);

        SyncClientConfig sc_config;
        sc_config.base_file_path = util::make_temp_dir();
        sc_config.metadata_mode = realm::SyncManager::MetadataMode::NoMetadata;

        // initialize app and sync client
        auto redir_app = app::App::get_app(app::App::CacheMode::Disabled, app_config, sc_config);

        int request_count = 0;
        const std::string original_url = get_base_url();
        std::string original_host = original_url.substr(original_url.find("://") + 3);
        original_host = original_host.substr(0, original_host.find("/"));
        std::string original_ws_host = util::format("ws://%1", original_host);
        const std::string redirect_url = "http://fakerealm.example.com:9090";
        redir_transport->request_hook = [&](const Request& request) -> std::optional<Response> {
            logger->trace("request.url (%1): %2", request_count, request.url);
            if (request_count++ == 0) {
                // First request should be to location
                REQUIRE(request.url.find("/location") != std::string::npos);
                logger->trace("original_url (%1): %2", request_count, original_url);
            }
            else if (request_count++ == 1) {
                REQUIRE(!request.redirect_count);
                return Response{
                    308, 0, {{"Location", redirect_url}, {"Content-Type", "application/json"}}, "Some body data"};
            }
            else if (request_count++ == 2) {
                REQUIRE(request.url.find("location") != std::string::npos);
                // app hostname will be updated via the metadata info
                return Response{
                    static_cast<int>(sync::HTTPStatus::Ok),
                    0,
                    {{"Content-Type", "application/json"}},
                    util::format("{\"deployment_model\":\"GLOBAL\",\"location\":\"US-VA\",\"hostname\":\"%1\",\"ws_"
                                 "hostname\":\"%2\"}",
                                 original_url, original_ws_host)};
            }
            else {
                REQUIRE(request.url.find(original_url) != std::string::npos);
            }
            return std::nullopt;
        };

        // This will be successful after a couple of retries due to the redirect response
        redir_app->provider_client<app::App::UsernamePasswordProviderClient>().register_email(
            creds.email, creds.password, [&](util::Optional<app::AppError> error) {
                REQUIRE(!error);
            });
        REQUIRE(redir_app->sync_manager()->sync_route());
        REQUIRE(redir_app->sync_manager()->sync_route()->find(original_ws_host) != std::string::npos);

        // Register another email address and verify location data isn't requested again
        request_count = 0;
        redir_transport->request_hook = [&](const Request& request) -> std::optional<Response> {
            logger->trace("request.url (%1): %2", request_count, request.url);
            REQUIRE(request.url.find("location") == std::string::npos);
            request_count++;
            return std::nullopt;
        };

        redir_app->provider_client<app::App::UsernamePasswordProviderClient>().register_email(
            creds2.email, creds2.password, [&](util::Optional<app::AppError> error) {
                REQUIRE(!error);
            });
    }

    SECTION("Test websocket redirect with existing session") {
        std::string configured_app_url = get_base_url();
        std::string original_host = configured_app_url.substr(configured_app_url.find("://") + 3);
        original_host = original_host.substr(0, original_host.find("/"));
        std::string original_address = original_host;
        uint16_t original_port = 443;
        if (auto port_pos = original_host.find(":"); port_pos != std::string::npos) {
            auto original_port_str = original_host.substr(port_pos + 1);

            original_port = strtol(original_port_str.c_str(), nullptr, 10);
            original_address = original_host.substr(0, port_pos);
        }

        std::string redirect_scheme = "http://";
        std::string websocket_scheme = "ws://";
        const std::string redirect_address = "fakerealm.example.com";
        const std::string redirect_host = "fakerealm.example.com:9090";
        const std::string redirect_url = "http://fakerealm.example.com:9090";

        auto redir_transport = std::make_shared<HookedTransport>();
        auto redir_provider = std::make_shared<HookedSocketProvider>(logger, "");
        redir_provider->websocket_endpoint_resolver = [&](sync::WebSocketEndpoint&& ep) {
            ep.address = original_address;
            ep.port = original_port;
            return ep;
        };
        std::mutex logout_mutex;
        std::condition_variable logout_cv;
        bool logged_out = false;

        auto server_app_config = minimal_app_config("websocket_redirect", schema);
        TestAppSession test_session(create_app(server_app_config), redir_transport, DeleteApp{true},
                                    realm::ReconnectMode::normal, redir_provider);
        auto partition = random_string(100);
        auto user1 = test_session.app()->current_user();
        SyncTestFile r_config(user1, partition, schema);
        // Override the default
        r_config.sync_config->error_handler = [&](std::shared_ptr<SyncSession>, SyncError error) {
            if (error.status == ErrorCodes::AuthError) {
                util::format(std::cerr, "Websocket redirect test: User logged out\n");
                std::unique_lock lk(logout_mutex);
                logged_out = true;
                logout_cv.notify_one();
                return;
            }
            util::format(std::cerr, "An unexpected sync error was caught by the default SyncTestFile handler: '%1'\n",
                         error.status);
            abort();
        };

        auto r = Realm::get_shared_realm(r_config);

        REQUIRE(!wait_for_download(*r));

        SECTION("Valid websocket redirect") {
            auto sync_manager = test_session.sync_manager();
            auto sync_session = sync_manager->get_existing_session(r->config().path);
            sync_session->pause();

            int connect_count = 0;
            redir_provider->websocket_connect_simulated_response_func = [&connect_count]() -> std::optional<int> {
                if (connect_count++ > 0)
                    return std::nullopt;

                return static_cast<int>(sync::HTTPStatus::PermanentRedirect);
            };
            redir_provider->websocket_endpoint_resolver = [&](sync::WebSocketEndpoint&& ep) {
                if (connect_count < 2) {
                    return ep;
                }
                REQUIRE(ep.address == redirect_address);
                ep.address = original_address;
                ep.port = original_port;
                return ep;
            };
            int request_count = 0;
            redir_transport->request_hook = [&](const Request& request) -> std::optional<Response> {
                logger->trace("request.url (%1): %2", request_count, request.url);
                if (request_count++ == 0) {
                    // First request should be a location request against the original URL
                    REQUIRE(request.url.find(original_host) != std::string::npos);
                    REQUIRE(request.url.find("/location") != std::string::npos);
                    REQUIRE(request.redirect_count == 0);
                    return Response{static_cast<int>(sync::HTTPStatus::PermanentRedirect),
                                    0,
                                    {{"Location", redirect_url}, {"Content-Type", "application/json"}},
                                    "Some body data"};
                }
                else if (request.url.find("/location") != std::string::npos) {
                    REQUIRE(request.url.find(redirect_host) != std::string::npos);
                    ++request_count;
                    return Response{
                        static_cast<int>(sync::HTTPStatus::Ok),
                        0,
                        {{"Content-Type", "application/json"}},
                        util::format(
                            "{\"deployment_model\":\"GLOBAL\",\"location\":\"US-VA\",\"hostname\":\"%2%1\",\"ws_"
                            "hostname\":\"%3%1\"}",
                            redirect_host, redirect_scheme, websocket_scheme)};
                }
                else if (request.url.find(redirect_host) != std::string::npos) {
                    auto new_req = request;
                    new_req.url = util::format("%1%2", configured_app_url, request.url.substr(redirect_url.size()));
                    logger->trace("Proxying request from %1 to %2", request.url, new_req.url);
                    auto resp = do_http_request(new_req);
                    logger->trace("Response: \"%1\"", resp.body);
                    return resp;
                }
                return std::nullopt;
            };

            SyncManager::OnlyForTesting::voluntary_disconnect_all_connections(*sync_manager);
            sync_session->resume();
            REQUIRE(!wait_for_download(*r));
            REQUIRE(user1->is_logged_in());

            // Verify session is using the updated server url from the redirect
            auto server_url = sync_session->full_realm_url();
            logger->trace("FULL_REALM_URL: %1", server_url);
            REQUIRE((server_url && server_url->find(redirect_host) != std::string::npos));
        }
        SECTION("Websocket redirect logs out user") {
            auto sync_manager = test_session.sync_manager();
            auto sync_session = sync_manager->get_existing_session(r->config().path);
            sync_session->pause();

            int connect_count = 0;
            redir_provider->websocket_connect_simulated_response_func = [&connect_count]() -> std::optional<int> {
                if (connect_count++ > 0)
                    return std::nullopt;

                return static_cast<int>(sync::HTTPStatus::MovedPermanently);
            };
            int request_count = 0;
            redir_transport->request_hook = [&](const Request& request) -> std::optional<Response> {
                logger->trace("request.url (%1): %2", request_count, request.url);
                if (request_count++ == 0) {
                    // First request should be a location request against the original URL
                    REQUIRE(request.url.find(original_host) != std::string::npos);
                    REQUIRE(request.url.find("/location") != std::string::npos);
                    REQUIRE(request.redirect_count == 0);
                    return Response{static_cast<int>(sync::HTTPStatus::MovedPermanently),
                                    0,
                                    {{"Location", redirect_url}, {"Content-Type", "application/json"}},
                                    "Some body data"};
                }
                else if (request.url.find("/location") != std::string::npos) {
                    return Response{
                        static_cast<int>(sync::HTTPStatus::Ok),
                        0,
                        {{"Content-Type", "application/json"}},
                        util::format(
                            "{\"deployment_model\":\"GLOBAL\",\"location\":\"US-VA\",\"hostname\":\"%2%1\",\"ws_"
                            "hostname\":\"%3%1\"}",
                            redirect_host, redirect_scheme, websocket_scheme)};
                }
                else if (request.url.find("auth/session") != std::string::npos) {
                    return Response{static_cast<int>(sync::HTTPStatus::Unauthorized),
                                    0,
                                    {{"Content-Type", "application/json"}},
                                    ""};
                }
                return std::nullopt;
            };

            SyncManager::OnlyForTesting::voluntary_disconnect_all_connections(*sync_manager);
            sync_session->resume();
            REQUIRE(wait_for_download(*r));
            std::unique_lock lk(logout_mutex);
            auto result = logout_cv.wait_for(lk, std::chrono::seconds(15), [&]() {
                return logged_out;
            });
            REQUIRE(result);
            REQUIRE(!user1->is_logged_in());
        }
        SECTION("Too many websocket redirects logs out user") {
            auto sync_manager = test_session.sync_manager();
            auto sync_session = sync_manager->get_existing_session(r->config().path);
            sync_session->pause();

            int connect_count = 0;
            redir_provider->websocket_connect_simulated_response_func = [&connect_count]() -> std::optional<int> {
                if (connect_count++ > 0)
                    return std::nullopt;

                return static_cast<int>(sync::HTTPStatus::MovedPermanently);
            };
            int request_count = 0;
            const int max_http_redirects = 20; // from app.cpp in object-store
            redir_transport->request_hook = [&](const Request& request) -> std::optional<Response> {
                logger->trace("request.url (%1): %2", request_count, request.url);
                if (request_count++ == 0) {
                    // First request should be a location request against the original URL
                    REQUIRE(request.url.find(original_host) != std::string::npos);
                    REQUIRE(request.url.find("/location") != std::string::npos);
                    REQUIRE(request.redirect_count == 0);
                }
                if (request.url.find("/location") != std::string::npos) {
                    // Keep returning the redirected response
                    REQUIRE(request.redirect_count < max_http_redirects);
                    return Response{static_cast<int>(sync::HTTPStatus::MovedPermanently),
                                    0,
                                    {{"Location", redirect_url}, {"Content-Type", "application/json"}},
                                    "Some body data"};
                }
                else {
                    FAIL("should not get any other types of requests during the test - the log out is local");
                }
                return std::nullopt;
            };

            SyncManager::OnlyForTesting::voluntary_disconnect_all_connections(*sync_manager);
            sync_session->resume();
            REQUIRE(wait_for_download(*r));
            std::unique_lock lk(logout_mutex);
            auto result = logout_cv.wait_for(lk, std::chrono::seconds(15), [&]() {
                return logged_out;
            });
            REQUIRE(result);
            REQUIRE(!user1->is_logged_in());
        }
    }

    SECTION("Fast clock on client") {
        {
            SyncTestFile config(app, partition, schema);
            auto r = Realm::get_shared_realm(config);

            REQUIRE(get_dogs(r).size() == 0);
            create_one_dog(r);
            REQUIRE(get_dogs(r).size() == 1);
        }

        auto transport = std::make_shared<HookedTransport>();
        TestAppSession hooked_session(session.app_session(), transport, DeleteApp{false});
        auto app = hooked_session.app();
        std::shared_ptr<SyncUser> user = app->current_user();
        REQUIRE(user);
        REQUIRE(!user->access_token_refresh_required());
        // Make the SyncUser behave as if the client clock is 31 minutes fast, so the token looks expired locally
        // (access tokens have an lifetime of 30 minutes today).
        user->set_seconds_to_adjust_time_for_testing(31 * 60);
        REQUIRE(user->access_token_refresh_required());

        // This assumes that we make an http request for the new token while
        // already in the WaitingForAccessToken state.
        bool seen_waiting_for_access_token = false;
        transport->request_hook = [&](const Request&) -> std::optional<Response> {
            auto user = app->current_user();
            REQUIRE(user);
            for (auto& session : user->all_sessions()) {
                // Prior to the fix for #4941, this callback would be called from an infinite loop, always in the
                // WaitingForAccessToken state.
                if (session->state() == SyncSession::State::WaitingForAccessToken) {
                    REQUIRE(!seen_waiting_for_access_token);
                    seen_waiting_for_access_token = true;
                }
            }
            return std::nullopt;
        };
        SyncTestFile config(app, partition, schema);
        auto r = Realm::get_shared_realm(config);
        REQUIRE(seen_waiting_for_access_token);
        Results dogs = get_dogs(r);
        REQUIRE(dogs.size() == 1);
        REQUIRE(dogs.get(0).get<String>("breed") == "bulldog");
        REQUIRE(dogs.get(0).get<String>("name") == "fido");
    }

    SECTION("Expired Tokens") {
        sync::AccessToken token;
        {
            std::shared_ptr<SyncUser> user = app->current_user();
            SyncTestFile config(app, partition, schema);
            auto r = Realm::get_shared_realm(config);

            REQUIRE(get_dogs(r).size() == 0);
            create_one_dog(r);

            REQUIRE(get_dogs(r).size() == 1);
            sync::AccessToken::ParseError error_state = realm::sync::AccessToken::ParseError::none;
            sync::AccessToken::parse(user->access_token(), token, error_state, nullptr);
            REQUIRE(error_state == sync::AccessToken::ParseError::none);
            REQUIRE(token.timestamp);
            REQUIRE(token.expires);
            REQUIRE(token.timestamp < token.expires);
            std::chrono::system_clock::time_point now = std::chrono::system_clock::now();
            using namespace std::chrono_literals;
            token.expires = std::chrono::system_clock::to_time_t(now - 30s);
            REQUIRE(token.expired(now));
        }

        auto transport = std::make_shared<HookedTransport>();
        TestAppSession hooked_session(session.app_session(), transport, DeleteApp{false});
        auto app = hooked_session.app();
        std::shared_ptr<SyncUser> user = app->current_user();
        REQUIRE(user);
        REQUIRE(!user->access_token_refresh_required());
        // Set a bad access token, with an expired time. This will trigger a refresh initiated by the client.
        user->update_access_token(encode_fake_jwt("fake_access_token", token.expires, token.timestamp));
        REQUIRE(user->access_token_refresh_required());

        SECTION("Expired Access Token is Refreshed") {
            // This assumes that we make an http request for the new token while
            // already in the WaitingForAccessToken state.
            bool seen_waiting_for_access_token = false;
            transport->request_hook = [&](const Request&) -> std::optional<Response> {
                auto user = app->current_user();
                REQUIRE(user);
                for (auto& session : user->all_sessions()) {
                    if (session->state() == SyncSession::State::WaitingForAccessToken) {
                        REQUIRE(!seen_waiting_for_access_token);
                        seen_waiting_for_access_token = true;
                    }
                }
                return std::nullopt;
            };
            SyncTestFile config(app, partition, schema);
            auto r = Realm::get_shared_realm(config);
            REQUIRE(seen_waiting_for_access_token);
            Results dogs = get_dogs(r);
            REQUIRE(dogs.size() == 1);
            REQUIRE(dogs.get(0).get<String>("breed") == "bulldog");
            REQUIRE(dogs.get(0).get<String>("name") == "fido");
        }

        SECTION("User is logged out if the refresh request is denied") {
            REQUIRE(user->is_logged_in());
            transport->response_hook = [&](const Request& request, const Response& response) {
                auto user = app->current_user();
                REQUIRE(user);
                // simulate the server denying the refresh
                if (request.url.find("/session") != std::string::npos) {
                    auto& response_ref = const_cast<Response&>(response);
                    response_ref.http_status_code = 401;
                    response_ref.body = "fake: refresh token could not be refreshed";
                }
            };
            SyncTestFile config(app, partition, schema);
            std::atomic<bool> sync_error_handler_called{false};
            config.sync_config->error_handler = [&](std::shared_ptr<SyncSession>, SyncError error) {
                sync_error_handler_called.store(true);
                REQUIRE(error.status.code() == ErrorCodes::AuthError);
                REQUIRE_THAT(std::string{error.status.reason()},
                             Catch::Matchers::StartsWith("Unable to refresh the user access token"));
            };
            auto r = Realm::get_shared_realm(config);
            timed_wait_for([&] {
                return sync_error_handler_called.load();
            });
            // the failed refresh logs out the user
            REQUIRE(!user->is_logged_in());
        }

        SECTION("User is left logged out if logged out while the refresh is in progress") {
            REQUIRE(user->is_logged_in());
            transport->request_hook = [&](const Request&) -> std::optional<Response> {
                user->log_out();
                return std::nullopt;
            };
            SyncTestFile config(app, partition, schema);
            auto r = Realm::get_shared_realm(config);
            REQUIRE_FALSE(user->is_logged_in());
            REQUIRE(user->state() == SyncUser::State::LoggedOut);
        }

        SECTION("Requests that receive an error are retried on a backoff") {
            using namespace std::chrono;
            std::vector<time_point<steady_clock>> response_times;
            std::atomic<bool> did_receive_valid_token{false};
            constexpr size_t num_error_responses = 6;

            transport->response_hook = [&](const Request& request, const Response& response) {
                // simulate the server experiencing an internal server error
                if (request.url.find("/session") != std::string::npos) {
                    if (response_times.size() >= num_error_responses) {
                        did_receive_valid_token.store(true);
                        return;
                    }
                    auto& response_ref = const_cast<Response&>(response);
                    response_ref.http_status_code = 500;
                }
            };
            transport->request_hook = [&](const Request& request) -> std::optional<Response> {
                if (!did_receive_valid_token.load() && request.url.find("/session") != std::string::npos) {
                    response_times.push_back(steady_clock::now());
                }
                return std::nullopt;
            };
            SyncTestFile config(app, partition, schema);
            auto r = Realm::get_shared_realm(config);
            create_one_dog(r);
            timed_wait_for(
                [&] {
                    return did_receive_valid_token.load();
                },
                30s);
            REQUIRE(user->is_logged_in());
            REQUIRE(response_times.size() >= num_error_responses);
            std::vector<uint64_t> delay_times;
            for (size_t i = 1; i < response_times.size(); ++i) {
                delay_times.push_back(duration_cast<milliseconds>(response_times[i] - response_times[i - 1]).count());
            }

            // sync delays start at 1000ms minus a random number of up to 25%.
            // the subsequent delay is double the previous one minus a random 25% again.
            // this calculation happens in Connection::initiate_reconnect_wait()
            bool increasing_delay = true;
            for (size_t i = 1; i < delay_times.size(); ++i) {
                if (delay_times[i - 1] >= delay_times[i]) {
                    increasing_delay = false;
                }
            }
            // fail if the first delay isn't longer than half a second
            if (delay_times.size() <= 1 || delay_times[1] < 500) {
                increasing_delay = false;
            }
            if (!increasing_delay) {
                std::cerr << "delay times are not increasing: ";
                for (auto& delay : delay_times) {
                    std::cerr << delay << ", ";
                }
                std::cerr << std::endl;
            }
            REQUIRE(increasing_delay);
        }
    }

    SECTION("Invalid refresh token") {
        auto& app_session = session.app_session();
        std::mutex mtx;
        auto verify_error_on_sync_with_invalid_refresh_token = [&](std::shared_ptr<SyncUser> user,
                                                                   Realm::Config config) {
            REQUIRE(user);
            REQUIRE(app_session.admin_api.verify_access_token(user->access_token(), app_session.server_app_id));

            // requesting a new access token fails because the refresh token used for this request is revoked
            user->refresh_custom_data([&](Optional<AppError> error) {
                REQUIRE(error);
                REQUIRE(error->additional_status_code == 401);
                REQUIRE(error->code() == ErrorCodes::InvalidSession);
            });

            // Set a bad access token. This will force a request for a new access token when the sync session opens
            // this is only necessary because the server doesn't actually revoke previously issued access tokens
            // instead allowing their session to time out as normal. So this simulates the access token expiring.
            // see:
            // https://github.com/10gen/baas/blob/05837cc3753218dfaf89229c6930277ef1616402/api/common/auth.go#L1380-L1386
            user->update_access_token(encode_fake_jwt("fake_access_token"));
            REQUIRE(!app_session.admin_api.verify_access_token(user->access_token(), app_session.server_app_id));

            auto [sync_error_promise, sync_error] = util::make_promise_future<SyncError>();
            config.sync_config->error_handler =
                [promise = util::CopyablePromiseHolder(std::move(sync_error_promise))](std::shared_ptr<SyncSession>,
                                                                                       SyncError error) mutable {
                    promise.get_promise().emplace_value(std::move(error));
                };

            auto transport = static_cast<SynchronousTestTransport*>(session.transport());
            transport->block(); // don't let the token refresh happen until we're ready for it
            auto r = Realm::get_shared_realm(config);
            auto session = user->session_for_on_disk_path(config.path);
            REQUIRE(user->is_logged_in());
            REQUIRE(!sync_error.is_ready());
            {
                std::atomic<bool> called{false};
                session->wait_for_upload_completion([&](Status stat) {
                    std::lock_guard lock(mtx);
                    called.store(true);
                    REQUIRE(stat.code() == ErrorCodes::InvalidSession);
                });
                transport->unblock();
                timed_wait_for([&] {
                    return called.load();
                });
                std::lock_guard lock(mtx);
                REQUIRE(called);
            }

            auto sync_error_res = wait_for_future(std::move(sync_error)).get();
            REQUIRE(sync_error_res.status == ErrorCodes::AuthError);
            REQUIRE_THAT(std::string{sync_error_res.status.reason()},
                         Catch::Matchers::StartsWith("Unable to refresh the user access token"));

            // the failed refresh logs out the user
            std::lock_guard lock(mtx);
            REQUIRE(!user->is_logged_in());
        };

        SECTION("Disabled user results in a sync error") {
            auto creds = create_user_and_log_in(app);
            SyncTestFile config(app, partition, schema);
            auto user = app->current_user();
            REQUIRE(user);
            REQUIRE(app_session.admin_api.verify_access_token(user->access_token(), app_session.server_app_id));
            app_session.admin_api.disable_user_sessions(app->current_user()->identity(), app_session.server_app_id);

            verify_error_on_sync_with_invalid_refresh_token(user, config);

            // logging in again doesn't fix things while the account is disabled
            auto error = failed_log_in(app, creds);
            REQUIRE(error.code() == ErrorCodes::UserDisabled);

            // admin enables user sessions again which should allow the session to continue
            app_session.admin_api.enable_user_sessions(user->identity(), app_session.server_app_id);

            // logging in now works properly
            log_in(app, creds);

            // still referencing the same user
            REQUIRE(user == app->current_user());
            REQUIRE(user->is_logged_in());

            {
                // check that there are no errors initiating a session now by making sure upload/download succeeds
                auto r = Realm::get_shared_realm(config);
                Results dogs = get_dogs(r);
            }
        }

        SECTION("Revoked refresh token results in a sync error") {
            auto creds = create_user_and_log_in(app);
            SyncTestFile config(app, partition, schema);
            auto user = app->current_user();
            REQUIRE(app_session.admin_api.verify_access_token(user->access_token(), app_session.server_app_id));
            app_session.admin_api.revoke_user_sessions(user->identity(), app_session.server_app_id);
            // revoking a user session only affects the refresh token, so the access token should still continue to
            // work.
            REQUIRE(app_session.admin_api.verify_access_token(user->access_token(), app_session.server_app_id));

            verify_error_on_sync_with_invalid_refresh_token(user, config);

            // logging in again succeeds and generates a new and valid refresh token
            log_in(app, creds);

            // still referencing the same user and now the user is logged in
            REQUIRE(user == app->current_user());
            REQUIRE(user->is_logged_in());

            // new requests for an access token succeed again
            user->refresh_custom_data([&](Optional<AppError> error) {
                REQUIRE_FALSE(error);
            });

            {
                // check that there are no errors initiating a new sync session by making sure upload/download
                // succeeds
                auto r = Realm::get_shared_realm(config);
                Results dogs = get_dogs(r);
            }
        }

        SECTION("Revoked refresh token on an anonymous user results in a sync error") {
            app->current_user()->log_out();
            auto anon_user = log_in(app);
            REQUIRE(app->current_user() == anon_user);
            SyncTestFile config(app, partition, schema);
            REQUIRE(app_session.admin_api.verify_access_token(anon_user->access_token(), app_session.server_app_id));
            app_session.admin_api.revoke_user_sessions(anon_user->identity(), app_session.server_app_id);
            // revoking a user session only affects the refresh token, so the access token should still continue to
            // work.
            REQUIRE(app_session.admin_api.verify_access_token(anon_user->access_token(), app_session.server_app_id));

            verify_error_on_sync_with_invalid_refresh_token(anon_user, config);

            // the user has been logged out, and current user is reset
            REQUIRE(!app->current_user());
            REQUIRE(!anon_user->is_logged_in());
            REQUIRE(anon_user->state() == SyncUser::State::Removed);

            // new requests for an access token do not work for anon users
            anon_user->refresh_custom_data([&](Optional<AppError> error) {
                REQUIRE(error);
                REQUIRE(error->reason() ==
                        util::format("Cannot initiate a refresh on user '%1' because the user has been removed",
                                     anon_user->identity()));
            });

            REQUIRE_EXCEPTION(
                Realm::get_shared_realm(config), ClientUserNotFound,
                util::format("Cannot start a sync session for user '%1' because this user has been removed.",
                             anon_user->identity()));
        }

        SECTION("Opening a Realm with a removed email user results produces an exception") {
            auto creds = create_user_and_log_in(app);
            auto email_user = app->current_user();
            const std::string user_ident = email_user->identity();
            REQUIRE(email_user);
            SyncTestFile config(app, partition, schema);
            REQUIRE(email_user->is_logged_in());
            {
                // sync works on a valid user
                auto r = Realm::get_shared_realm(config);
                Results dogs = get_dogs(r);
            }
            app->sync_manager()->remove_user(user_ident);
            REQUIRE_FALSE(email_user->is_logged_in());
            REQUIRE(email_user->state() == SyncUser::State::Removed);

            // should not be able to open a synced Realm with an invalid user
            REQUIRE_EXCEPTION(
                Realm::get_shared_realm(config), ClientUserNotFound,
                util::format("Cannot start a sync session for user '%1' because this user has been removed.",
                             user_ident));

            std::shared_ptr<SyncUser> new_user_instance = log_in(app, creds);
            // the previous instance is still invalid
            REQUIRE_FALSE(email_user->is_logged_in());
            REQUIRE(email_user->state() == SyncUser::State::Removed);
            // but the new instance will work and has the same server issued ident
            REQUIRE(new_user_instance);
            REQUIRE(new_user_instance->is_logged_in());
            REQUIRE(new_user_instance->identity() == user_ident);
            {
                // sync works again if the same user is logged back in
                config.sync_config->user = new_user_instance;
                auto r = Realm::get_shared_realm(config);
                Results dogs = get_dogs(r);
            }
        }
    }

    SECTION("large write transactions which would be too large if batched") {
        SyncTestFile config(app, partition, schema);

        std::mutex mutex;
        bool done = false;
        auto r = Realm::get_shared_realm(config);
        r->sync_session()->pause();

        // Create 26 MB worth of dogs in 26 transactions, which should work but
        // will result in an error from the server if the changesets are batched
        // for upload.
        CppContext c;
        for (auto i = 'a'; i < 'z'; ++i) {
            r->begin_transaction();
            Object::create(c, r, "Dog",
                           std::any(AnyDict{{"_id", std::any(ObjectId::gen())},
                                            {"breed", std::string("bulldog")},
                                            {"name", random_string(1024 * 1024)}}),
                           CreatePolicy::ForceCreate);
            r->commit_transaction();
        }
        r->sync_session()->wait_for_upload_completion([&](Status status) {
            std::lock_guard lk(mutex);
            REQUIRE(status.is_ok());
            done = true;
        });
        r->sync_session()->resume();

        // If we haven't gotten an error in more than 5 minutes, then something has gone wrong
        // and we should fail the test.
        timed_wait_for(
            [&] {
                std::lock_guard lk(mutex);
                return done;
            },
            std::chrono::minutes(5));
    }

    SECTION("too large sync message error handling") {
        SyncTestFile config(app, partition, schema);

        auto pf = util::make_promise_future<SyncError>();
        config.sync_config->error_handler =
            [sp = util::CopyablePromiseHolder(std::move(pf.promise))](auto, SyncError error) mutable {
                sp.get_promise().emplace_value(std::move(error));
            };
        auto r = Realm::get_shared_realm(config);

        // Create 26 MB worth of dogs in a single transaction - this should all get put into one changeset
        // and get uploaded at once, which for now is an error on the server.
        r->begin_transaction();
        CppContext c;
        for (auto i = 'a'; i < 'z'; ++i) {
            Object::create(c, r, "Dog",
                           std::any(AnyDict{{"_id", std::any(ObjectId::gen())},
                                            {"breed", std::string("bulldog")},
                                            {"name", random_string(1024 * 1024)}}),
                           CreatePolicy::ForceCreate);
        }
        r->commit_transaction();

#if defined(TEST_TIMEOUT_EXTRA) && TEST_TIMEOUT_EXTRA > 0
        // It may take 30 minutes to transfer 16MB at 10KB/s
        auto delay = std::chrono::minutes(35);
#else
        auto delay = std::chrono::minutes(5);
#endif

        auto error = wait_for_future(std::move(pf.future), delay).get();
        REQUIRE(error.status == ErrorCodes::LimitExceeded);
        REQUIRE(error.status.reason() ==
                "Sync websocket closed because the server received a message that was too large: "
                "read limited at 16777217 bytes");
        REQUIRE(error.is_client_reset_requested());
        REQUIRE(error.server_requests_action == sync::ProtocolErrorInfo::Action::ClientReset);
    }

    SECTION("freezing realm does not resume session") {
        SyncTestFile config(app, partition, schema);
        auto realm = Realm::get_shared_realm(config);
        wait_for_download(*realm);

        auto state = realm->sync_session()->state();
        REQUIRE(state == SyncSession::State::Active);

        realm->sync_session()->pause();
        state = realm->sync_session()->state();
        REQUIRE(state == SyncSession::State::Paused);

        realm->read_group();

        {
            auto frozen = realm->freeze();
            REQUIRE(realm->sync_session() == realm->sync_session());
            REQUIRE(realm->sync_session()->state() == SyncSession::State::Paused);
        }

        {
            auto frozen = Realm::get_frozen_realm(config, realm->read_transaction_version());
            REQUIRE(realm->sync_session() == realm->sync_session());
            REQUIRE(realm->sync_session()->state() == SyncSession::State::Paused);
        }
    }

    SECTION("pausing a session does not hold the DB open") {
        SyncTestFile config(app, partition, schema);
        DBRef dbref;
        std::shared_ptr<SyncSession> sync_sess_ext_ref;
        {
            auto realm = Realm::get_shared_realm(config);
            wait_for_download(*realm);

            auto state = realm->sync_session()->state();
            REQUIRE(state == SyncSession::State::Active);

            sync_sess_ext_ref = realm->sync_session()->external_reference();
            dbref = TestHelper::get_db(*realm);
            // One ref each for the
            // - RealmCoordinator
            // - SyncSession
            // - SessionWrapper
            // - local dbref
            REQUIRE(dbref.use_count() >= 4);

            realm->sync_session()->pause();
            state = realm->sync_session()->state();
            REQUIRE(state == SyncSession::State::Paused);
        }

        // Closing the realm should leave one ref for the SyncSession and one for the local dbref.
        REQUIRE_THAT(
            [&] {
                return dbref.use_count() < 4;
            },
            ReturnsTrueWithinTimeLimit{});

        // Releasing the external reference should leave one ref (the local dbref) only.
        sync_sess_ext_ref.reset();
        REQUIRE_THAT(
            [&] {
                return dbref.use_count() == 1;
            },
            ReturnsTrueWithinTimeLimit{});
    }

    SECTION("validation") {
        SyncTestFile config(app, partition, schema);

        SECTION("invalid partition error handling") {
            config.sync_config->partition_value = "not a bson serialized string";
            std::atomic<bool> error_did_occur = false;
            config.sync_config->error_handler = [&error_did_occur](std::shared_ptr<SyncSession>, SyncError error) {
                CHECK(error.status.reason().find(
                          "Illegal Realm path (BIND): serialized partition 'not a bson serialized "
                          "string' is invalid") != std::string::npos);
                error_did_occur.store(true);
            };
            auto r = Realm::get_shared_realm(config);
            auto session = app->current_user()->session_for_on_disk_path(r->config().path);
            timed_wait_for([&] {
                return error_did_occur.load();
            });
            REQUIRE(error_did_occur.load());
        }

        SECTION("invalid pk schema error handling") {
            const std::string invalid_pk_name = "my_primary_key";
            auto it = config.schema->find("Dog");
            REQUIRE(it != config.schema->end());
            REQUIRE(it->primary_key_property());
            REQUIRE(it->primary_key_property()->name == "_id");
            it->primary_key_property()->name = invalid_pk_name;
            it->primary_key = invalid_pk_name;
            REQUIRE_THROWS_CONTAINING(Realm::get_shared_realm(config),
                                      "The primary key property on a synchronized Realm must be named '_id' but "
                                      "found 'my_primary_key' for type 'Dog'");
        }

        SECTION("missing pk schema error handling") {
            auto it = config.schema->find("Dog");
            REQUIRE(it != config.schema->end());
            REQUIRE(it->primary_key_property());
            it->primary_key_property()->is_primary = false;
            it->primary_key = "";
            REQUIRE(!it->primary_key_property());
            REQUIRE_THROWS_CONTAINING(Realm::get_shared_realm(config),
                                      "There must be a primary key property named '_id' on a synchronized "
                                      "Realm but none was found for type 'Dog'");
        }
    }

    SECTION("get_file_ident") {
        SyncTestFile config(app, partition, schema);
        config.sync_config->client_resync_mode = ClientResyncMode::RecoverOrDiscard;
        auto r = Realm::get_shared_realm(config);
        wait_for_download(*r);

        auto first_ident = r->sync_session()->get_file_ident();
        REQUIRE(first_ident.ident != 0);
        REQUIRE(first_ident.salt != 0);

        reset_utils::trigger_client_reset(session.app_session(), r);
        r->sync_session()->restart_session();
        wait_for_download(*r);

        REQUIRE(first_ident.ident != r->sync_session()->get_file_ident().ident);
        REQUIRE(first_ident.salt != r->sync_session()->get_file_ident().salt);
    }
}

TEST_CASE("app: base_url", "[sync][app][base_url]") {

    struct BaseUrlTransport : GenericNetworkTransport {
        std::string expected_url;
        std::optional<std::string> redirect_url;
        bool location_requested = false;
        bool location_returns_error = false;

        void reset(std::string expect_url, std::optional<std::string> redir_url = std::nullopt)
        {
            expected_url = expect_url;
            redirect_url = redir_url;
            location_requested = false;
            location_returns_error = false;
        }

        void send_request_to_server(const Request& request,
                                    util::UniqueFunction<void(const Response&)>&& completion) override
        {
            if (request.url.find("/login") != std::string::npos) {
                CHECK(request.url.find(expected_url) != std::string::npos);
                completion({200, 0, {}, user_json(good_access_token).dump()});
            }
            else if (request.url.find("/profile") != std::string::npos) {
                CHECK(request.url.find(expected_url) != std::string::npos);
                completion({200, 0, {}, user_profile_json().dump()});
            }
            else if (request.url.find("/session") != std::string::npos && request.method == HttpMethod::post) {
                nlohmann::json json{{"access_token", good_access_token}};
                CHECK(request.url.find(expected_url) != std::string::npos);
                completion({200, 0, {}, json.dump()});
            }
            else if (request.url.find("/location") != std::string::npos) {
                CHECK(request.method == HttpMethod::get);
                CHECK(request.url.find(expected_url) != std::string::npos);
                location_requested = true;
                if (location_returns_error) {
                    completion(app::Response{static_cast<int>(sync::HTTPStatus::NotFound), 0, {}, "404 not found"});
                    return;
                }
                if (redirect_url) {
                    // Update the expected url to be the redirect url
                    expected_url = *redirect_url;
                    redirect_url.reset();

                    completion(app::Response{static_cast<int>(sync::HTTPStatus::PermanentRedirect),
                                             0,
                                             {{"location", expected_url}},
                                             "308 permanent redirect"});
                    return;
                }
                auto ws_url = expected_url;
                ws_url.replace(0, 4, "ws");
                completion(
                    app::Response{static_cast<int>(sync::HTTPStatus::Ok),
                                  0,
                                  {},
                                  util::format("{\"deployment_model\":\"GLOBAL\",\"location\":\"US-VA\",\"hostname\":"
                                               "\"%1\",\"ws_hostname\":\"%2\"}",
                                               expected_url, ws_url)});
            }
        }
    };

    std::unique_ptr<realm::AppSession> app_session;
    auto redir_transport = std::make_shared<BaseUrlTransport>();
    AutoVerifiedEmailCredentials creds;
    auto logger = util::Logger::get_default_logger();

    App::Config app_config = {"fake-app-id"};
    set_app_config_defaults(app_config, redir_transport);

    SyncClientConfig sc_config;
    sc_config.base_file_path = util::make_temp_dir();
    sc_config.metadata_mode = realm::SyncManager::MetadataMode::NoEncryption;
    sc_config.logger_factory = [](util::Logger::Level) {
        return util::Logger::get_default_logger();
    };

    auto do_login = [&](std::shared_ptr<app::App> app) {
        CHECK(app);
        app->log_in_with_credentials(realm::app::AppCredentials::username_password(creds.email, creds.password),
                                     [](std::shared_ptr<realm::SyncUser> user, util::Optional<app::AppError> error) {
                                         REQUIRE(user);
                                         REQUIRE(!error);
                                     });
    };

    SECTION("Test app config baseurl") {
        {
            redir_transport->reset("https://realm.mongodb.com");

            // First time through, base_url is empty; https://realm.mongodb.com is expected
            auto app = app::App::get_app(app::App::CacheMode::Disabled, app_config, sc_config);
            // Location is not requested until first app services request
            CHECK(!redir_transport->location_requested);
            // Initial hostname and ws hostname use base url, but aren't used until location is updated
            CHECK(app->get_host_url() == "https://realm.mongodb.com");
            CHECK(app->get_ws_host_url() == "wss://realm.mongodb.com");

            do_login(app);
            CHECK(redir_transport->location_requested);
            CHECK(app->get_base_url() == "https://realm.mongodb.com");
            CHECK(app->get_host_url() == "https://realm.mongodb.com");
            CHECK(app->get_ws_host_url() == "wss://realm.mongodb.com");
        }
        {
            // Second time through, base_url is set to https://alternate.someurl.fake is expected
            app_config.base_url = "https://alternate.someurl.fake";
            redir_transport->reset("https://alternate.someurl.fake");

            auto app = app::App::get_app(app::App::CacheMode::Disabled, app_config, sc_config);
            // Location is not requested until first app services request
            CHECK(!redir_transport->location_requested);
            // Initial hostname and ws hostname use base url, but aren't used until location is updated
            CHECK(app->get_host_url() == "https://alternate.someurl.fake");
            CHECK(app->get_ws_host_url() == "wss://alternate.someurl.fake");

            do_login(app);
            CHECK(redir_transport->location_requested);
            CHECK(app->get_base_url() == "https://alternate.someurl.fake");
            CHECK(app->get_host_url() == "https://alternate.someurl.fake");
            CHECK(app->get_ws_host_url() == "wss://alternate.someurl.fake");
        }
        {
            // Third time through, base_url is not set, expect https://realm.mongodb.com, since metadata
            // is no longer used
            app_config.base_url = util::none;
            std::string expected_url = "https://realm.mongodb.com";
            std::string expected_wsurl = "wss://realm.mongodb.com";
            redir_transport->reset(expected_url);

            auto app = app::App::get_app(app::App::CacheMode::Disabled, app_config, sc_config);
            // Location is not requested until first app services request
            CHECK(!redir_transport->location_requested);
            // Initial hostname and ws hostname use base url, but aren't used until location is updated
            CHECK(app->get_host_url() == expected_url);
            CHECK(app->get_ws_host_url() == expected_wsurl);

            do_login(app);
            CHECK(redir_transport->location_requested);
            CHECK(app->get_base_url() == expected_url);
            CHECK(app->get_host_url() == expected_url);
            CHECK(app->get_ws_host_url() == expected_wsurl);
        }
        {
            // Fourth time through, base_url is set to https://some-other.someurl.fake, with a redirect
            app_config.base_url = "https://some-other.someurl.fake";
            redir_transport->reset("https://some-other.someurl.fake", "http://redirect.someurl.fake");

            auto app = app::App::get_app(app::App::CacheMode::Disabled, app_config, sc_config);
            // Location is not requested until first app services request
            CHECK(!redir_transport->location_requested);
            // Initial hostname and ws hostname use base url, but aren't used until location is updated
            CHECK(app->get_host_url() == "https://some-other.someurl.fake");
            CHECK(app->get_ws_host_url() == "wss://some-other.someurl.fake");

            do_login(app);
            CHECK(redir_transport->location_requested);
            // Base URL is still set to the original value
            CHECK(app->get_base_url() == "https://some-other.someurl.fake");
            // Hostname and ws hostname use the redirect URL values
            CHECK(app->get_host_url() == "http://redirect.someurl.fake");
            CHECK(app->get_ws_host_url() == "ws://redirect.someurl.fake");
        }
    }

    SECTION("Test update_baseurl") {
        {
            app_config.base_url = "https://alternate.someurl.fake";
            redir_transport->reset("https://alternate.someurl.fake");

            auto app = app::App::get_app(app::App::CacheMode::Disabled, app_config, sc_config);
            // Location is not requested until first app services request
            CHECK(!redir_transport->location_requested);

            do_login(app);
            CHECK(redir_transport->location_requested);
            CHECK(app->get_base_url() == "https://alternate.someurl.fake");
            CHECK(app->get_host_url() == "https://alternate.someurl.fake");
            CHECK(app->get_ws_host_url() == "wss://alternate.someurl.fake");

            redir_transport->reset("https://realm.mongodb.com");

            // Revert the base URL to the default URL value using std::nullopt
            app->update_base_url(std::nullopt, [](util::Optional<app::AppError> error) {
                CHECK(!error);
            });
            CHECK(redir_transport->location_requested);
            CHECK(app->get_base_url() == "https://realm.mongodb.com");
            CHECK(app->get_host_url() == "https://realm.mongodb.com");
            CHECK(app->get_ws_host_url() == "wss://realm.mongodb.com");
            // Expected URL is still "https://realm.mongodb.com"
            do_login(app);

            redir_transport->reset("http://some-other.url.fake");
            app->update_base_url("http://some-other.url.fake", [](util::Optional<app::AppError> error) {
                CHECK(!error);
            });
            CHECK(redir_transport->location_requested);
            CHECK(app->get_base_url() == "http://some-other.url.fake");
            CHECK(app->get_host_url() == "http://some-other.url.fake");
            CHECK(app->get_ws_host_url() == "ws://some-other.url.fake");
            // Expected URL is still "http://some-other.url.fake"
            do_login(app);

            redir_transport->reset("https://realm.mongodb.com");

            // Revert the base URL to the default URL value using the empty string
            app->update_base_url("", [](util::Optional<app::AppError> error) {
                CHECK(!error);
            });
            CHECK(redir_transport->location_requested);
            CHECK(app->get_base_url() == "https://realm.mongodb.com");
            CHECK(app->get_host_url() == "https://realm.mongodb.com");
            CHECK(app->get_ws_host_url() == "wss://realm.mongodb.com");
            // Expected URL is still "https://realm.mongodb.com"
            do_login(app);
        }
    }

    SECTION("Test update_baseurl with redirect") {
        {
            app_config.base_url = "https://alternate.someurl.fake";
            redir_transport->reset("https://alternate.someurl.fake");

            auto app = app::App::get_app(app::App::CacheMode::Disabled, app_config, sc_config);
            // Location is not requested until first app services request
            CHECK(!redir_transport->location_requested);

            do_login(app);
            CHECK(redir_transport->location_requested);
            CHECK(app->get_base_url() == "https://alternate.someurl.fake");
            CHECK(app->get_host_url() == "https://alternate.someurl.fake");
            CHECK(app->get_ws_host_url() == "wss://alternate.someurl.fake");

            redir_transport->reset("http://some-other.someurl.fake", "https://redirect.otherurl.fake");

            app->update_base_url("http://some-other.someurl.fake", [](util::Optional<app::AppError> error) {
                CHECK(!error);
            });
            CHECK(redir_transport->location_requested);
            CHECK(app->get_base_url() == "http://some-other.someurl.fake");
            CHECK(app->get_host_url() == "https://redirect.otherurl.fake");
            CHECK(app->get_ws_host_url() == "wss://redirect.otherurl.fake");
            // Expected URL is still "https://redirect.otherurl.fake" after redirect
            do_login(app);
        }
    }

    SECTION("Test update_baseurl returns error") {
        {
            app_config.base_url = "http://alternate.someurl.fake";
            redir_transport->reset("http://alternate.someurl.fake");

            auto app = app::App::get_app(app::App::CacheMode::Disabled, app_config, sc_config);
            // Location is not requested until first app services request
            CHECK(!redir_transport->location_requested);

            do_login(app);
            CHECK(redir_transport->location_requested);
            CHECK(app->get_base_url() == "http://alternate.someurl.fake");
            CHECK(app->get_host_url() == "http://alternate.someurl.fake");
            CHECK(app->get_ws_host_url() == "ws://alternate.someurl.fake");

            redir_transport->reset("https://some-other.someurl.fake");
            redir_transport->location_returns_error = true;

            app->update_base_url("https://some-other.someurl.fake", [](util::Optional<app::AppError> error) {
                CHECK(error);
            });
            CHECK(redir_transport->location_requested);
            // Verify original url values are still being used
            CHECK(app->get_base_url() == "http://alternate.someurl.fake");
            CHECK(app->get_host_url() == "http://alternate.someurl.fake");
            CHECK(app->get_ws_host_url() == "ws://alternate.someurl.fake");
        }
    }

    // Verify new sync session updates location after app created with cached user
    SECTION("Verify new sync session updates location") {
        bool use_ssl = GENERATE(true, false);
        std::string expected_host = "redirect.someurl.fake";
        unsigned expected_port = 8081;
        std::string init_url = util::format("http%1://alternate.someurl.fake", use_ssl ? "s" : "");
        std::string init_wsurl = util::format("ws%1://alternate.someurl.fake", use_ssl ? "s" : "");
        std::string redir_url = util::format("http%1://%2:%3", use_ssl ? "s" : "", expected_host, expected_port);
        std::string redir_wsurl = util::format("ws%1://%2:%3", use_ssl ? "s" : "", expected_host, expected_port);

        auto socket_provider = std::make_shared<HookedSocketProvider>(logger, "some user agent");
        socket_provider->endpoint_verify_func = [&use_ssl, &expected_host,
                                                 &expected_port](sync::WebSocketEndpoint& ep) {
            CHECK(ep.address == expected_host);
            CHECK(ep.port == expected_port);
            CHECK(ep.is_ssl == use_ssl);
        };
        socket_provider->force_failure_func = [](bool& was_clean, sync::websocket::WebSocketError& error_code,
                                                 std::string& message) {
            was_clean = false;
            error_code = sync::websocket::WebSocketError::websocket_connection_failed;
            message = "404 not found";
            return true;
        };

        sc_config.socket_provider = socket_provider;

        app_config.base_url = init_url;

        // Log in to get a cached user
        {
            redir_transport->reset(init_url);

            auto app = app::App::get_app(app::App::CacheMode::Disabled, app_config, sc_config);
            // At this point, the sync route is not set
            CHECK(!app->sync_manager()->sync_route());

            do_login(app);
            CHECK(redir_transport->location_requested);
            CHECK(app->get_base_url() == init_url);
            CHECK(app->get_host_url() == init_url);
            CHECK(app->get_ws_host_url() == init_wsurl);
            CHECK(app->sync_manager()->sync_route());
            CHECK(app->sync_manager()->sync_route()->find(init_wsurl) != std::string::npos);
        }
        // Recreate the app using the cached user and start a sync session, which will is set to fail on connect
        SECTION("Sync Session fails on connect") {
            enum class TestState { start, session_started };
            TestingStateMachine<TestState> state(TestState::start);

            redir_transport->reset(init_url, redir_url);

            auto app = app::App::get_app(app::App::CacheMode::Disabled, app_config, sc_config);
            // At this point, the sync route is not set
            CHECK(!app->sync_manager()->sync_route());

            RealmConfig r_config;
            r_config.path = sc_config.base_file_path + "/fakerealm.realm";
            r_config.sync_config = std::make_shared<SyncConfig>(app->current_user(), SyncConfig::FLXSyncEnabled{});
            r_config.sync_config->error_handler = [&state, &logger](std::shared_ptr<SyncSession>,
                                                                    SyncError error) mutable {
                // Expect an error due to 404 response when creating websocket
                state.transition_with([&error, &logger](TestState cur_state) -> std::optional<TestState> {
                    if (cur_state == TestState::start) {
                        // The session will start, but the connection is rejected on purpose
                        logger->debug("Expected error: %1", error.status);
                        CHECK(!error.status.is_ok());
                        CHECK(error.status.code() == ErrorCodes::SyncConnectFailed);
                        return TestState::session_started;
                    }
                    return std::nullopt;
                });
            };
            auto realm = Realm::get_shared_realm(r_config);
            state.wait_for(TestState::session_started);

            CHECK(redir_transport->location_requested);
            CHECK(app->get_base_url() == init_url);
            CHECK(app->get_host_url() == redir_url);
            CHECK(app->get_ws_host_url() == redir_wsurl);
            CHECK(app->sync_manager()->sync_route());
            CHECK(app->sync_manager()->sync_route()->find(redir_wsurl) != std::string::npos);
        }
        // Recreate the app using the cached user and start a sync session, which will fail during location update
        SECTION("Location update fails prior to sync session connect") {
            enum class TestState { start, location_failed, waiting_for_session, session_started };
            TestingStateMachine<TestState> state(TestState::start);

            redir_transport->reset(init_url, redir_url);
            redir_transport->location_returns_error = true;

            auto app = app::App::get_app(app::App::CacheMode::Disabled, app_config, sc_config);
            // At this point, the sync route is not set
            CHECK(!app->sync_manager()->sync_route());

            RealmConfig r_config;
            r_config.path = sc_config.base_file_path + "/fakerealm.realm";
            r_config.sync_config = std::make_shared<SyncConfig>(app->current_user(), SyncConfig::FLXSyncEnabled{});
            r_config.sync_config->error_handler = [&state, &logger](std::shared_ptr<SyncSession>,
                                                                    SyncError error) mutable {
                // Expect an error due to location failed or 404 response when creating websocket
                state.transition_with([&error, &logger](TestState cur_state) -> std::optional<TestState> {
                    if (cur_state == TestState::start || cur_state == TestState::waiting_for_session) {
                        logger->debug("Expected error: %1: %2", error.status.code_string(), error.status.reason());
                        CHECK(!error.status.is_ok());
                        CHECK(error.status.code() == ErrorCodes::SyncConnectFailed);
                    }
                    if (cur_state == TestState::start) {
                        // The first time through, the location update fails
                        return TestState::location_failed;
                    }
                    else if (cur_state == TestState::waiting_for_session) {
                        // The second time through, the session start, but the connection is rejected on purpose
                        return TestState::session_started;
                    }
                    return std::nullopt;
                });
            };
            auto realm = Realm::get_shared_realm(r_config);
            state.wait_for(TestState::location_failed);

            CHECK(redir_transport->location_requested);
            CHECK(app->get_base_url() == init_url);
            // Location was never updated
            CHECK(app->get_host_url() == init_url);
            CHECK(app->get_ws_host_url() == init_wsurl);
            CHECK(!app->sync_manager()->sync_route());

            // Location request will pass this time, try to reconnect
            // expecting 404 when websocket connects
            redir_transport->reset(init_url, redir_url);
            state.transition_to(TestState::waiting_for_session);
            auto session = app->sync_manager()->get_existing_session(r_config.path);
            CHECK(session);
            session->resume();
            state.wait_for(TestState::session_started);

            CHECK(redir_transport->location_requested);
            CHECK(app->get_base_url() == init_url);
            CHECK(app->get_host_url() == redir_url);
            CHECK(app->get_ws_host_url() == redir_wsurl);
            CHECK(app->sync_manager()->sync_route());
            CHECK(app->sync_manager()->sync_route()->find(redir_wsurl) != std::string::npos);
        }
    }
}

TEST_CASE("app: custom user data integration tests", "[sync][app][user][function][baas]") {
    TestAppSession session;
    auto app = session.app();
    auto user = app->current_user();

    SECTION("custom user data happy path") {
        bool processed = false;
        app->call_function("updateUserData", {bson::BsonDocument({{"favorite_color", "green"}})},
                           [&](auto response, auto error) {
                               CHECK(error == none);
                               CHECK(response);
                               CHECK(*response == true);
                               processed = true;
                           });
        CHECK(processed);
        processed = false;
        app->refresh_custom_data(user, [&](auto) {
            processed = true;
        });
        CHECK(processed);
        auto data = *user->custom_data();
        CHECK(data["favorite_color"] == "green");
    }
}

TEST_CASE("app: jwt login and metadata tests", "[sync][app][user][metadata][function][baas]") {
    TestAppSession session;
    auto app = session.app();
    auto jwt = create_jwt(session.app()->config().app_id);

    SECTION("jwt happy path") {
        bool processed = false;

        std::shared_ptr<SyncUser> user = log_in(app, AppCredentials::custom(jwt));

        app->call_function(user, "updateUserData", {bson::BsonDocument({{"name", "Not Foo Bar"}})},
                           [&](auto response, auto error) {
                               CHECK(error == none);
                               CHECK(response);
                               CHECK(*response == true);
                               processed = true;
                           });
        CHECK(processed);
        processed = false;
        app->refresh_custom_data(user, [&](auto) {
            processed = true;
        });
        CHECK(processed);
        auto metadata = user->user_profile();
        auto custom_data = *user->custom_data();
        CHECK(custom_data["name"] == "Not Foo Bar");
        CHECK(metadata["name"] == "Foo Bar");
    }
}

namespace cf = realm::collection_fixtures;
TEMPLATE_TEST_CASE("app: collections of links integration", "[sync][pbs][app][collections][baas]", cf::ListOfObjects,
                   cf::ListOfMixedLinks, cf::SetOfObjects, cf::SetOfMixedLinks, cf::DictionaryOfObjects,
                   cf::DictionaryOfMixedLinks)
{
    const std::string valid_pk_name = "_id";
    const auto partition = random_string(100);
    TestType test_type("collection", "dest");
    Schema schema = {{"source",
                      {{valid_pk_name, PropertyType::Int | PropertyType::Nullable, true},
                       {"realm_id", PropertyType::String | PropertyType::Nullable},
                       test_type.property()}},
                     {"dest",
                      {
                          {valid_pk_name, PropertyType::Int | PropertyType::Nullable, true},
                          {"realm_id", PropertyType::String | PropertyType::Nullable},
                      }}};
    auto server_app_config = minimal_app_config("collections_of_links", schema);
    TestAppSession test_session(create_app(server_app_config));

    auto wait_for_num_objects_to_equal = [](realm::SharedRealm r, const std::string& table_name, size_t count) {
        timed_sleeping_wait_for([&]() -> bool {
            r->refresh();
            TableRef dest = r->read_group().get_table(table_name);
            size_t cur_count = dest->size();
            return cur_count == count;
        });
    };
    auto wait_for_num_outgoing_links_to_equal = [&](realm::SharedRealm r, Obj obj, size_t count) {
        timed_sleeping_wait_for([&]() -> bool {
            r->refresh();
            return test_type.size_of_collection(obj) == count;
        });
    };

    CppContext c;
    auto create_one_source_object = [&](realm::SharedRealm r, int64_t val, std::vector<ObjLink> links = {}) {
        r->begin_transaction();
        auto object = Object::create(
            c, r, "source",
            std::any(realm::AnyDict{{valid_pk_name, std::any(val)}, {"realm_id", std::string(partition)}}),
            CreatePolicy::ForceCreate);

        for (auto link : links) {
            auto& obj = object.get_obj();
            test_type.add_link(obj, link);
        }
        r->commit_transaction();
        return object;
    };

    auto create_one_dest_object = [&](realm::SharedRealm r, int64_t val) -> ObjLink {
        r->begin_transaction();
        auto obj = Object::create(
            c, r, "dest",
            std::any(realm::AnyDict{{valid_pk_name, std::any(val)}, {"realm_id", std::string(partition)}}),
            CreatePolicy::ForceCreate);
        r->commit_transaction();
        return ObjLink{obj.get_obj().get_table()->get_key(), obj.get_obj().get_key()};
    };

    auto require_links_to_match_ids = [&](std::vector<Obj> links, std::vector<int64_t> expected) {
        std::vector<int64_t> actual;
        for (auto obj : links) {
            actual.push_back(obj.get<Int>(valid_pk_name));
        }
        std::sort(actual.begin(), actual.end());
        std::sort(expected.begin(), expected.end());
        REQUIRE(actual == expected);
    };

    SECTION("integration testing") {
        auto app = test_session.app();
        SyncTestFile config1(app, partition, schema); // uses the current user created above
        config1.automatic_change_notifications = false;
        auto r1 = realm::Realm::get_shared_realm(config1);
        Results r1_source_objs = realm::Results(r1, r1->read_group().get_table("class_source"));

        create_user_and_log_in(app);
        SyncTestFile config2(app, partition, schema); // uses the user created above
        config2.automatic_change_notifications = false;
        auto r2 = realm::Realm::get_shared_realm(config2);
        Results r2_source_objs = realm::Results(r2, r2->read_group().get_table("class_source"));

        constexpr int64_t source_pk = 0;
        constexpr int64_t dest_pk_1 = 1;
        constexpr int64_t dest_pk_2 = 2;
        constexpr int64_t dest_pk_3 = 3;
        Object object;

        { // add a container collection with three valid links
            REQUIRE(r1_source_objs.size() == 0);
            ObjLink dest1 = create_one_dest_object(r1, dest_pk_1);
            ObjLink dest2 = create_one_dest_object(r1, dest_pk_2);
            ObjLink dest3 = create_one_dest_object(r1, dest_pk_3);
            object = create_one_source_object(r1, source_pk, {dest1, dest2, dest3});
            REQUIRE(r1_source_objs.size() == 1);
            REQUIRE(r1_source_objs.get(0).get<Int>(valid_pk_name) == source_pk);
            REQUIRE(r1_source_objs.get(0).get<String>("realm_id") == partition);
            require_links_to_match_ids(test_type.get_links(r1_source_objs.get(0)), {dest_pk_1, dest_pk_2, dest_pk_3});
        }

        size_t expected_coll_size = 3;
        std::vector<int64_t> remaining_dest_object_ids;
        { // erase one of the destination objects
            wait_for_num_objects_to_equal(r2, "class_source", 1);
            wait_for_num_objects_to_equal(r2, "class_dest", 3);
            REQUIRE(r2_source_objs.size() == 1);
            REQUIRE(r2_source_objs.get(0).get<Int>(valid_pk_name) == source_pk);
            REQUIRE(test_type.size_of_collection(r2_source_objs.get(0)) == 3);
            auto linked_objects = test_type.get_links(r2_source_objs.get(0));
            require_links_to_match_ids(linked_objects, {dest_pk_1, dest_pk_2, dest_pk_3});
            r2->begin_transaction();
            linked_objects[0].remove();
            r2->commit_transaction();
            remaining_dest_object_ids = {linked_objects[1].template get<Int>(valid_pk_name),
                                         linked_objects[2].template get<Int>(valid_pk_name)};
            expected_coll_size = test_type.will_erase_removed_object_links() ? 2 : 3;
            REQUIRE(test_type.size_of_collection(r2_source_objs.get(0)) == expected_coll_size);
        }

        { // remove a link from the collection
            wait_for_num_objects_to_equal(r1, "class_dest", 2);
            REQUIRE(r1_source_objs.size() == 1);
            REQUIRE(test_type.size_of_collection(r1_source_objs.get(0)) == expected_coll_size);
            auto linked_objects = test_type.get_links(r1_source_objs.get(0));
            require_links_to_match_ids(linked_objects, remaining_dest_object_ids);
            r1->begin_transaction();
            auto obj = r1_source_objs.get(0);
            test_type.remove_link(obj,
                                  ObjLink{linked_objects[0].get_table()->get_key(), linked_objects[0].get_key()});
            r1->commit_transaction();
            --expected_coll_size;
            remaining_dest_object_ids = {linked_objects[1].template get<Int>(valid_pk_name)};
            REQUIRE(test_type.size_of_collection(r1_source_objs.get(0)) == expected_coll_size);
        }
        bool coll_cleared = false;
        advance_and_notify(*r1);
        auto collection = test_type.get_collection(r1, r1_source_objs.get(0));
        auto token = collection.add_notification_callback([&coll_cleared](CollectionChangeSet c) {
            coll_cleared = c.collection_was_cleared;
        });

        { // clear the collection
            REQUIRE(r2_source_objs.size() == 1);
            REQUIRE(r2_source_objs.get(0).get<Int>(valid_pk_name) == source_pk);
            wait_for_num_outgoing_links_to_equal(r2, r2_source_objs.get(0), expected_coll_size);
            auto linked_objects = test_type.get_links(r2_source_objs.get(0));
            require_links_to_match_ids(linked_objects, remaining_dest_object_ids);
            r2->begin_transaction();
            test_type.clear_collection(r2_source_objs.get(0));
            r2->commit_transaction();
            expected_coll_size = 0;
            REQUIRE(test_type.size_of_collection(r2_source_objs.get(0)) == expected_coll_size);
        }

        { // expect an empty collection
            REQUIRE(!coll_cleared);
            REQUIRE(r1_source_objs.size() == 1);
            wait_for_num_outgoing_links_to_equal(r1, r1_source_objs.get(0), expected_coll_size);
            advance_and_notify(*r1);
            REQUIRE(coll_cleared);
        }
    }
}

TEMPLATE_TEST_CASE("app: partition types", "[sync][pbs][app][partition][baas]", cf::Int, cf::String, cf::OID,
                   cf::UUID, cf::BoxedOptional<cf::Int>, cf::UnboxedOptional<cf::String>, cf::BoxedOptional<cf::OID>,
                   cf::BoxedOptional<cf::UUID>)
{
    const std::string valid_pk_name = "_id";
    const std::string partition_key_col_name = "partition_key_prop";
    const std::string table_name = "class_partition_test_type";
    auto partition_property = Property(partition_key_col_name, TestType::property_type);
    Schema schema = {{Group::table_name_to_class_name(table_name),
                      {
                          {valid_pk_name, PropertyType::Int, true},
                          partition_property,
                      }}};
    auto server_app_config = minimal_app_config("partition_types_app_name", schema);
    server_app_config.partition_key = partition_property;
    TestAppSession test_session(create_app(server_app_config));
    auto app = test_session.app();

    auto wait_for_num_objects_to_equal = [](realm::SharedRealm r, const std::string& table_name, size_t count) {
        timed_sleeping_wait_for([&]() -> bool {
            r->refresh();
            TableRef dest = r->read_group().get_table(table_name);
            size_t cur_count = dest->size();
            return cur_count == count;
        });
    };
    using T = typename TestType::Type;
    CppContext c;
    auto create_object = [&](realm::SharedRealm r, int64_t val, std::any partition) {
        r->begin_transaction();
        auto object = Object::create(
            c, r, Group::table_name_to_class_name(table_name),
            std::any(realm::AnyDict{{valid_pk_name, std::any(val)}, {partition_key_col_name, partition}}),
            CreatePolicy::ForceCreate);
        r->commit_transaction();
    };

    auto get_bson = [](T val) -> bson::Bson {
        if constexpr (std::is_same_v<T, StringData>) {
            return val.is_null() ? bson::Bson(util::none) : bson::Bson(val);
        }
        else if constexpr (TestType::is_optional) {
            return val ? bson::Bson(*val) : bson::Bson(util::none);
        }
        else {
            return bson::Bson(val);
        }
    };

    SECTION("can round trip an object") {
        auto values = TestType::values();
        auto user1 = app->current_user();
        create_user_and_log_in(app);
        auto user2 = app->current_user();
        REQUIRE(user1);
        REQUIRE(user2);
        REQUIRE(user1 != user2);
        for (T partition_value : values) {
            SyncTestFile config1(user1, get_bson(partition_value), schema); // uses the current user created above
            auto r1 = realm::Realm::get_shared_realm(config1);
            Results r1_source_objs = realm::Results(r1, r1->read_group().get_table(table_name));

            SyncTestFile config2(user2, get_bson(partition_value), schema); // uses the user created above
            auto r2 = realm::Realm::get_shared_realm(config2);
            Results r2_source_objs = realm::Results(r2, r2->read_group().get_table(table_name));

            const int64_t pk_value = random_int();
            {
                REQUIRE(r1_source_objs.size() == 0);
                create_object(r1, pk_value, TestType::to_any(partition_value));
                REQUIRE(r1_source_objs.size() == 1);
                REQUIRE(r1_source_objs.get(0).get<T>(partition_key_col_name) == partition_value);
                REQUIRE(r1_source_objs.get(0).get<Int>(valid_pk_name) == pk_value);
            }
            {
                wait_for_num_objects_to_equal(r2, table_name, 1);
                REQUIRE(r2_source_objs.size() == 1);
                REQUIRE(r2_source_objs.size() == 1);
                REQUIRE(r2_source_objs.get(0).get<T>(partition_key_col_name) == partition_value);
                REQUIRE(r2_source_objs.get(0).get<Int>(valid_pk_name) == pk_value);
            }
        }
    }
}

TEST_CASE("app: full-text compatible with sync", "[sync][app][baas]") {
    const std::string valid_pk_name = "_id";

    Schema schema{
        {"TopLevel",
         {
             {valid_pk_name, PropertyType::ObjectId, Property::IsPrimary{true}},
             {"full_text", Property::IsFulltextIndexed{true}},
         }},
    };

    auto server_app_config = minimal_app_config("full_text", schema);
    auto app_session = create_app(server_app_config);
    const auto partition = random_string(100);
    TestAppSession test_session(app_session, nullptr);
    SyncTestFile config(test_session.app(), partition, schema);
    SharedRealm realm;
    SECTION("sync open") {
        INFO("realm opened without async open");
        realm = Realm::get_shared_realm(config);
    }
    SECTION("async open") {
        INFO("realm opened with async open");
        auto async_open_task = Realm::get_synchronized_realm(config);

        auto [realm_promise, realm_future] = util::make_promise_future<ThreadSafeReference>();
        async_open_task->start(
            [promise = std::move(realm_promise)](ThreadSafeReference ref, std::exception_ptr ouch) mutable {
                if (ouch) {
                    try {
                        std::rethrow_exception(ouch);
                    }
                    catch (...) {
                        promise.set_error(exception_to_status());
                    }
                }
                else {
                    promise.emplace_value(std::move(ref));
                }
            });

        realm = Realm::get_shared_realm(std::move(realm_future.get()));
    }

    CppContext c(realm);
    auto obj_id_1 = ObjectId::gen();
    auto obj_id_2 = ObjectId::gen();
    realm->begin_transaction();
    Object::create(c, realm, "TopLevel", std::any(AnyDict{{"_id", obj_id_1}, {"full_text", "Hello, world!"s}}));
    Object::create(c, realm, "TopLevel", std::any(AnyDict{{"_id", obj_id_2}, {"full_text", "Hello, everyone!"s}}));
    realm->commit_transaction();

    auto table = realm->read_group().get_table("class_TopLevel");
    REQUIRE(table->search_index_type(table->get_column_key("full_text")) == IndexType::Fulltext);
    Results world_results(realm, Query(table).fulltext(table->get_column_key("full_text"), "world"));
    REQUIRE(world_results.size() == 1);
    REQUIRE(world_results.get<Obj>(0).get_primary_key() == Mixed{obj_id_1});
}

#endif // REALM_ENABLE_AUTH_TESTS

TEST_CASE("app: custom error handling", "[sync][app][custom errors]") {
    class CustomErrorTransport : public GenericNetworkTransport {
    public:
        CustomErrorTransport(int code, const std::string& message)
            : m_code(code)
            , m_message(message)
        {
        }

        void send_request_to_server(const Request&, util::UniqueFunction<void(const Response&)>&& completion) override
        {
            completion(Response{0, m_code, HttpHeaders(), m_message});
        }

    private:
        int m_code;
        std::string m_message;
    };

    SECTION("custom code and message is sent back") {
        OfflineAppSession::Config config;
        config.transport = std::make_shared<CustomErrorTransport>(1001, "Boom!");
        OfflineAppSession oas(config);
        auto error = failed_log_in(oas.app());
        CHECK(error.is_custom_error());
        CHECK(*error.additional_status_code == 1001);
        CHECK(error.reason() == "Boom!");
    }
}

// MARK: - Unit Tests

static const std::string bad_access_token = "lolwut";
static const std::string dummy_device_id = "123400000000000000000000";

TEST_CASE("subscribable unit tests", "[sync][app]") {
    struct Foo : public Subscribable<Foo> {
        void event()
        {
            emit_change_to_subscribers(*this);
        }
    };

    auto foo = Foo();

    SECTION("subscriber receives events") {
        auto event_count = 0;
        auto token = foo.subscribe([&event_count](auto&) {
            event_count++;
        });

        foo.event();
        foo.event();
        foo.event();

        CHECK(event_count == 3);
    }

    SECTION("subscriber can unsubscribe") {
        auto event_count = 0;
        auto token = foo.subscribe([&event_count](auto&) {
            event_count++;
        });

        foo.event();
        CHECK(event_count == 1);

        foo.unsubscribe(token);
        foo.event();
        CHECK(event_count == 1);
    }

    SECTION("subscriber is unsubscribed on dtor") {
        auto event_count = 0;
        {
            auto token = foo.subscribe([&event_count](auto&) {
                event_count++;
            });

            foo.event();
            CHECK(event_count == 1);
        }
        foo.event();
        CHECK(event_count == 1);
    }

    SECTION("multiple subscribers receive events") {
        auto event_count = 0;
        {
            auto token1 = foo.subscribe([&event_count](auto&) {
                event_count++;
            });
            auto token2 = foo.subscribe([&event_count](auto&) {
                event_count++;
            });

            foo.event();
            CHECK(event_count == 2);
        }
        foo.event();
        CHECK(event_count == 2);
    }
}

TEST_CASE("app: login_with_credentials unit_tests", "[sync][app][user]") {
    OfflineAppSession::Config config{std::make_shared<UnitTestTransport>()};
    static_cast<UnitTestTransport*>(config.transport.get())->set_profile(profile_0);

    SECTION("login_anonymous good") {
        UnitTestTransport::access_token = good_access_token;
        config.delete_storage = false;
        config.metadata_mode = SyncManager::MetadataMode::NoEncryption;
        config.storage_path = util::make_temp_dir();
        {
            OfflineAppSession oas(config);
            auto app = oas.app();

            auto user = log_in(app);

            REQUIRE(user->identities().size() == 1);
            CHECK(user->identities()[0].id == UnitTestTransport::identity_0_id);
            SyncUserProfile user_profile = user->user_profile();

            CHECK(user_profile.name() == profile_0_name);
            CHECK(user_profile.first_name() == profile_0_first_name);
            CHECK(user_profile.last_name() == profile_0_last_name);
            CHECK(user_profile.email() == profile_0_email);
            CHECK(user_profile.picture_url() == profile_0_picture_url);
            CHECK(user_profile.gender() == profile_0_gender);
            CHECK(user_profile.birthday() == profile_0_birthday);
            CHECK(user_profile.min_age() == profile_0_min_age);
            CHECK(user_profile.max_age() == profile_0_max_age);
        }
        App::clear_cached_apps();
        // assert everything is stored properly between runs
        {
            config.delete_storage = true; // clean up after this session
            OfflineAppSession oas(config);
            auto app = oas.app();
            REQUIRE(app->all_users().size() == 1);
            auto user = app->all_users()[0];
            REQUIRE(user->identities().size() == 1);
            CHECK(user->identities()[0].id == UnitTestTransport::identity_0_id);
            SyncUserProfile user_profile = user->user_profile();

            CHECK(user_profile.name() == profile_0_name);
            CHECK(user_profile.first_name() == profile_0_first_name);
            CHECK(user_profile.last_name() == profile_0_last_name);
            CHECK(user_profile.email() == profile_0_email);
            CHECK(user_profile.picture_url() == profile_0_picture_url);
            CHECK(user_profile.gender() == profile_0_gender);
            CHECK(user_profile.birthday() == profile_0_birthday);
            CHECK(user_profile.min_age() == profile_0_min_age);
            CHECK(user_profile.max_age() == profile_0_max_age);
        }
    }

    SECTION("login_anonymous bad") {
        struct transport : UnitTestTransport {
            void send_request_to_server(const Request& request,
                                        util::UniqueFunction<void(const Response&)>&& completion) override
            {
                if (request.url.find("/login") != std::string::npos) {
                    completion({200, 0, {}, user_json(bad_access_token).dump()});
                }
                else {
                    UnitTestTransport::send_request_to_server(request, std::move(completion));
                }
            }
        };

        config.transport = instance_of<transport>;
        OfflineAppSession oas(config);
        auto error = failed_log_in(oas.app());
        CHECK(error.reason() == std::string("malformed JWT"));
        CHECK(error.code_string() == "BadToken");
        CHECK(error.is_json_error());
        CHECK(error.code() == ErrorCodes::BadToken);
    }

    SECTION("login_anonynous multiple users") {
        UnitTestTransport::access_token = good_access_token;
        OfflineAppSession oas(config);
        auto app = oas.app();

        auto user1 = log_in(app);
        auto user2 = log_in(app, AppCredentials::anonymous(false));
        CHECK(user1 != user2);
    }
}

TEST_CASE("app: UserAPIKeyProviderClient unit_tests", "[sync][app][user][api key]") {
    OfflineAppSession oas({std::make_shared<UnitTestTransport>()});
    auto client = oas.app()->provider_client<App::UserAPIKeyProviderClient>();

    auto logged_in_user = oas.make_user();
    bool processed = false;
    ObjectId obj_id(UnitTestTransport::api_key_id.c_str());

    SECTION("create api key") {
        client.create_api_key(UnitTestTransport::api_key_name, logged_in_user,
                              [&](App::UserAPIKey user_api_key, Optional<AppError> error) {
                                  REQUIRE_FALSE(error);
                                  CHECK(user_api_key.disabled == false);
                                  CHECK(user_api_key.id.to_string() == UnitTestTransport::api_key_id);
                                  CHECK(user_api_key.key == UnitTestTransport::api_key);
                                  CHECK(user_api_key.name == UnitTestTransport::api_key_name);
                              });
    }

    SECTION("fetch api key") {
        client.fetch_api_key(obj_id, logged_in_user, [&](App::UserAPIKey user_api_key, Optional<AppError> error) {
            REQUIRE_FALSE(error);
            CHECK(user_api_key.disabled == false);
            CHECK(user_api_key.id.to_string() == UnitTestTransport::api_key_id);
            CHECK(user_api_key.name == UnitTestTransport::api_key_name);
        });
    }

    SECTION("fetch api keys") {
        client.fetch_api_keys(logged_in_user,
                              [&](std::vector<App::UserAPIKey> user_api_keys, Optional<AppError> error) {
                                  REQUIRE_FALSE(error);
                                  CHECK(user_api_keys.size() == 2);
                                  for (auto user_api_key : user_api_keys) {
                                      CHECK(user_api_key.disabled == false);
                                      CHECK(user_api_key.id.to_string() == UnitTestTransport::api_key_id);
                                      CHECK(user_api_key.name == UnitTestTransport::api_key_name);
                                  }
                                  processed = true;
                              });
        CHECK(processed);
    }
}


TEST_CASE("app: user_semantics", "[sync][app][user]") {
    OfflineAppSession oas(instance_of<UnitTestTransport>);
    auto app = oas.app();

    const auto login_user_email_pass = [=] {
        return log_in(app, AppCredentials::username_password("bob", "thompson"));
    };
    const auto login_user_anonymous = [=] {
        return log_in(app, AppCredentials::anonymous());
    };

    CHECK(!app->current_user());

    int event_processed = 0;
    auto token = app->subscribe([&event_processed](auto&) {
        event_processed++;
    });

    SECTION("current user is populated") {
        const auto user1 = login_user_anonymous();
        CHECK(app->current_user()->identity() == user1->identity());
        CHECK(event_processed == 1);
    }

    SECTION("current user is updated on login") {
        const auto user1 = login_user_anonymous();
        CHECK(app->current_user()->identity() == user1->identity());
        const auto user2 = login_user_email_pass();
        CHECK(app->current_user()->identity() == user2->identity());
        CHECK(user1->identity() != user2->identity());
        CHECK(event_processed == 2);
    }

    SECTION("current user is updated to last used user on logout") {
        const auto user1 = login_user_anonymous();
        CHECK(app->current_user()->identity() == user1->identity());
        CHECK(app->all_users()[0]->state() == SyncUser::State::LoggedIn);

        const auto user2 = login_user_email_pass();
        CHECK(app->all_users()[0]->state() == SyncUser::State::LoggedIn);
        CHECK(app->all_users()[1]->state() == SyncUser::State::LoggedIn);
        CHECK(app->current_user()->identity() == user2->identity());
        CHECK(user1 != user2);

        // should reuse existing session
        const auto user3 = login_user_anonymous();
        CHECK(user3 == user1);

        auto user_events_processed = 0;
        auto _ = user3->subscribe([&user_events_processed](auto&) {
            user_events_processed++;
        });

        app->log_out([](auto) {});
        CHECK(user_events_processed == 1);

        CHECK(app->current_user()->identity() == user2->identity());

        CHECK(app->all_users().size() == 1);
        CHECK(app->all_users()[0]->state() == SyncUser::State::LoggedIn);

        CHECK(event_processed == 4);
    }

    SECTION("anon users are removed on logout") {
        const auto user1 = login_user_anonymous();
        CHECK(app->current_user()->identity() == user1->identity());
        CHECK(app->all_users()[0]->state() == SyncUser::State::LoggedIn);

        const auto user2 = login_user_anonymous();
        CHECK(app->all_users()[0]->state() == SyncUser::State::LoggedIn);
        CHECK(app->all_users().size() == 1);
        CHECK(app->current_user()->identity() == user2->identity());
        CHECK(user1->identity() == user2->identity());

        app->log_out([](auto) {});
        CHECK(app->all_users().size() == 0);

        CHECK(event_processed == 3);
    }

    SECTION("logout user") {
        auto user1 = login_user_email_pass();
        auto user2 = login_user_anonymous();

        // Anonymous users are special
        app->log_out(user2, [](Optional<AppError> error) {
            REQUIRE_FALSE(error);
        });
        CHECK(user2->state() == SyncUser::State::Removed);

        // Other users can be LoggedOut
        app->log_out(user1, [](Optional<AppError> error) {
            REQUIRE_FALSE(error);
        });
        CHECK(user1->state() == SyncUser::State::LoggedOut);

        // Logging out already logged out users, does nothing
        app->log_out(user1, [](Optional<AppError> error) {
            REQUIRE_FALSE(error);
        });
        CHECK(user1->state() == SyncUser::State::LoggedOut);

        app->log_out(user2, [](Optional<AppError> error) {
            REQUIRE_FALSE(error);
        });
        CHECK(user2->state() == SyncUser::State::Removed);

        CHECK(event_processed == 4);
    }

    SECTION("unsubscribed observers no longer process events") {
        app->unsubscribe(token);

        const auto user1 = login_user_anonymous();
        CHECK(app->current_user()->identity() == user1->identity());
        CHECK(app->all_users()[0]->state() == SyncUser::State::LoggedIn);

        const auto user2 = login_user_anonymous();
        CHECK(app->all_users()[0]->state() == SyncUser::State::LoggedIn);
        CHECK(app->all_users().size() == 1);
        CHECK(app->current_user()->identity() == user2->identity());
        CHECK(user1->identity() == user2->identity());

        app->log_out([](auto) {});
        CHECK(app->all_users().size() == 0);

        CHECK(event_processed == 0);
    }
}

namespace {
struct ErrorCheckingTransport : public GenericNetworkTransport {
    ErrorCheckingTransport(Response* r)
        : m_response(r)
    {
    }
    void send_request_to_server(const Request& request,
                                util::UniqueFunction<void(const Response&)>&& completion) override
    {
        // Make sure to return a valid location response
        if (request.url.find("/location") != std::string::npos) {
            completion(Response{200,
                                0,
                                {{"content-type", "application/json"}},
                                "{\"deployment_model\":\"GLOBAL\",\"location\":\"US-VA\",\"hostname\":"
                                "\"http://some.fake.url\",\"ws_hostname\":\"ws://some.fake.url\"}"});
            return;
        }

        completion(Response(*m_response));
    }

private:
    Response* m_response;
};
} // namespace

TEST_CASE("app: response error handling", "[sync][app]") {
    std::string response_body = nlohmann::json({{"access_token", good_access_token},
                                                {"refresh_token", good_access_token},
                                                {"user_id", "Brown Bear"},
                                                {"device_id", "Panda Bear"}})
                                    .dump();

    Response response{200, 0, {{"Content-Type", "text/plain"}}, response_body};

    OfflineAppSession oas({std::make_shared<ErrorCheckingTransport>(&response)});
    auto app = oas.app();

    SECTION("http 404") {
        response.http_status_code = 404;
        auto error = failed_log_in(app);
        CHECK(!error.is_json_error());
        CHECK(!error.is_custom_error());
        CHECK(!error.is_service_error());
        CHECK(error.is_http_error());
        CHECK(*error.additional_status_code == 404);
        CHECK(error.reason().find(std::string("http error code considered fatal")) != std::string::npos);
    }
    SECTION("http 500") {
        response.http_status_code = 500;
        auto error = failed_log_in(app);
        CHECK(!error.is_json_error());
        CHECK(!error.is_custom_error());
        CHECK(!error.is_service_error());
        CHECK(error.is_http_error());
        CHECK(*error.additional_status_code == 500);
        CHECK(error.reason().find(std::string("http error code considered fatal")) != std::string::npos);
        CHECK(error.link_to_server_logs.empty());
    }

    SECTION("custom error code") {
        response.custom_status_code = 42;
        response.body = "Custom error message";
        auto error = failed_log_in(app);
        CHECK(!error.is_http_error());
        CHECK(!error.is_json_error());
        CHECK(!error.is_service_error());
        CHECK(error.is_custom_error());
        CHECK(*error.additional_status_code == 42);
        CHECK(error.reason() == std::string("Custom error message"));
        CHECK(error.link_to_server_logs.empty());
    }

    SECTION("session error code") {
        response.headers = HttpHeaders{{"Content-Type", "application/json"}};
        response.http_status_code = 400;
        response.body = nlohmann::json({{"error_code", "MongoDBError"},
                                        {"error", "a fake MongoDB error message!"},
                                        {"access_token", good_access_token},
                                        {"refresh_token", good_access_token},
                                        {"user_id", "Brown Bear"},
                                        {"device_id", "Panda Bear"},
                                        {"link", "http://...whatever the server passes us"}})
                            .dump();
        auto error = failed_log_in(app);
        CHECK(!error.is_http_error());
        CHECK(!error.is_json_error());
        CHECK(!error.is_custom_error());
        CHECK(error.is_service_error());
        CHECK(error.code() == ErrorCodes::MongoDBError);
        CHECK(error.reason() == std::string("a fake MongoDB error message!"));
        CHECK(error.link_to_server_logs == std::string("http://...whatever the server passes us"));
    }

    SECTION("json error code") {
        response.body = "this: is not{} a valid json body!";
        auto error = failed_log_in(app);
        CHECK(!error.is_http_error());
        CHECK(error.is_json_error());
        CHECK(!error.is_custom_error());
        CHECK(!error.is_service_error());
        CHECK(error.code() == ErrorCodes::MalformedJson);
        CHECK(error.reason() ==
              std::string("[json.exception.parse_error.101] parse error at line 1, column 2: syntax error "
                          "while parsing value - invalid literal; last read: 'th'"));
        CHECK(error.code_string() == "MalformedJson");
    }
}

TEST_CASE("app: switch user", "[sync][app][user]") {
    OfflineAppSession oas;
    auto app = oas.app();

    bool processed = false;

    SECTION("switch user expect success") {
        CHECK(app->all_users().size() == 0);

        // Log in user 1
        auto user_a = log_in(app, AppCredentials::username_password("test@10gen.com", "password"));
        CHECK(app->current_user() == user_a);

        // Log in user 2
        auto user_b = log_in(app, AppCredentials::username_password("test2@10gen.com", "password"));
        CHECK(app->current_user() == user_b);

        CHECK(app->all_users().size() == 2);

        app->switch_user(user_a);
        CHECK(app->current_user() == user_a);

        app->switch_user(user_b);

        CHECK(app->current_user() == user_b);
        processed = true;
        CHECK(processed);
    }

    SECTION("cannot switch to a logged out user") {
        CHECK(app->all_users().size() == 0);

        // Log in user 1
        auto user_a = log_in(app, AppCredentials::username_password("test@10gen.com", "password"));
        CHECK(app->current_user() == user_a);

        app->log_out([&](Optional<AppError> error) {
            REQUIRE_FALSE(error);
        });

        CHECK(app->current_user() == nullptr);
        CHECK(user_a->state() == SyncUser::State::LoggedOut);

        // Log in user 2
        auto user_b = log_in(app, AppCredentials::username_password("test2@10gen.com", "password"));
        CHECK(app->current_user() == user_b);
        CHECK(app->all_users().size() == 2);

        REQUIRE_THROWS_AS(app->switch_user(user_a), AppError);
        CHECK(app->current_user() == user_b);
    }
}

TEST_CASE("app: remove user", "[sync][app][user]") {
    OfflineAppSession oas;
    auto app = oas.app();

    SECTION("remove anonymous user") {
        CHECK(app->all_users().size() == 0);

        // Log in user 1
        auto user_a = log_in(app);
        CHECK(user_a->state() == SyncUser::State::LoggedIn);

        app->log_out(user_a, [&](Optional<AppError> error) {
            REQUIRE_FALSE(error);
            // a logged out anon user will be marked as Removed, not LoggedOut
            CHECK(user_a->state() == SyncUser::State::Removed);
        });
        CHECK(app->all_users().empty());

        app->remove_user(user_a, [&](Optional<AppError> error) {
            CHECK(error->reason() == "User has already been removed");
            CHECK(app->all_users().size() == 0);
        });

        // Log in user 2
        auto user_b = log_in(app);
        CHECK(app->current_user() == user_b);
        CHECK(user_b->state() == SyncUser::State::LoggedIn);
        CHECK(app->all_users().size() == 1);

        app->remove_user(user_b, [&](Optional<AppError> error) {
            REQUIRE_FALSE(error);
            CHECK(app->all_users().size() == 0);
        });

        CHECK(app->current_user() == nullptr);

        // check both handles are no longer valid
        CHECK(user_a->state() == SyncUser::State::Removed);
        CHECK(user_b->state() == SyncUser::State::Removed);
    }

    SECTION("remove user with credentials") {
        CHECK(app->all_users().size() == 0);
        CHECK(app->current_user() == nullptr);

        auto user = log_in(app, AppCredentials::username_password("email", "pass"));

        CHECK(user->state() == SyncUser::State::LoggedIn);

        app->log_out(user, [&](Optional<AppError> error) {
            REQUIRE_FALSE(error);
        });

        CHECK(user->state() == SyncUser::State::LoggedOut);

        app->remove_user(user, [&](Optional<AppError> error) {
            REQUIRE_FALSE(error);
        });
        CHECK(app->all_users().size() == 0);

        Optional<AppError> error;
        app->remove_user(user, [&](Optional<AppError> err) {
            error = err;
        });
        CHECK(error->code() > 0);
        CHECK(app->all_users().size() == 0);
        CHECK(user->state() == SyncUser::State::Removed);
    }
}

TEST_CASE("app: link_user", "[sync][app][user]") {
    OfflineAppSession oas;
    auto app = oas.app();

    auto email = util::format("realm_tests_do_autoverify%1@%2.com", random_string(10), random_string(10));
    auto password = random_string(10);

    auto custom_credentials = AppCredentials::facebook("a_token");
    auto email_pass_credentials = AppCredentials::username_password(email, password);

    auto sync_user = log_in(app, email_pass_credentials);
    REQUIRE(sync_user->identities().size() == 2);
    CHECK(sync_user->identities()[0].provider_type == IdentityProviderUsernamePassword);

    SECTION("successful link") {
        bool processed = false;
        app->link_user(sync_user, custom_credentials, [&](std::shared_ptr<SyncUser> user, Optional<AppError> error) {
            REQUIRE_FALSE(error);
            REQUIRE(user);
            CHECK(user->identity() == sync_user->identity());
            processed = true;
        });
        CHECK(processed);
    }

    SECTION("link_user should fail when logged out") {
        app->log_out([&](Optional<AppError> error) {
            REQUIRE_FALSE(error);
        });

        bool processed = false;
        app->link_user(sync_user, custom_credentials, [&](std::shared_ptr<SyncUser> user, Optional<AppError> error) {
            CHECK(error->reason() == "The specified user is not logged in.");
            CHECK(!user);
            processed = true;
        });
        CHECK(processed);
    }
}

TEST_CASE("app: auth providers", "[sync][app][user]") {
    SECTION("auth providers facebook") {
        auto credentials = AppCredentials::facebook("a_token");
        CHECK(credentials.provider() == AuthProvider::FACEBOOK);
        CHECK(credentials.provider_as_string() == IdentityProviderFacebook);
        CHECK(credentials.serialize_as_bson() ==
              bson::BsonDocument{{"provider", "oauth2-facebook"}, {"accessToken", "a_token"}});
    }

    SECTION("auth providers anonymous") {
        auto credentials = AppCredentials::anonymous();
        CHECK(credentials.provider() == AuthProvider::ANONYMOUS);
        CHECK(credentials.provider_as_string() == IdentityProviderAnonymous);
        CHECK(credentials.serialize_as_bson() == bson::BsonDocument{{"provider", "anon-user"}});
    }

    SECTION("auth providers anonymous no reuse") {
        auto credentials = AppCredentials::anonymous(false);
        CHECK(credentials.provider() == AuthProvider::ANONYMOUS_NO_REUSE);
        CHECK(credentials.provider_as_string() == IdentityProviderAnonymous);
        CHECK(credentials.serialize_as_bson() == bson::BsonDocument{{"provider", "anon-user"}});
    }

    SECTION("auth providers google authCode") {
        auto credentials = AppCredentials::google(AuthCode("a_token"));
        CHECK(credentials.provider() == AuthProvider::GOOGLE);
        CHECK(credentials.provider_as_string() == IdentityProviderGoogle);
        CHECK(credentials.serialize_as_bson() ==
              bson::BsonDocument{{"provider", "oauth2-google"}, {"authCode", "a_token"}});
    }

    SECTION("auth providers google idToken") {
        auto credentials = AppCredentials::google(IdToken("a_token"));
        CHECK(credentials.provider() == AuthProvider::GOOGLE);
        CHECK(credentials.provider_as_string() == IdentityProviderGoogle);
        CHECK(credentials.serialize_as_bson() ==
              bson::BsonDocument{{"provider", "oauth2-google"}, {"id_token", "a_token"}});
    }

    SECTION("auth providers apple") {
        auto credentials = AppCredentials::apple("a_token");
        CHECK(credentials.provider() == AuthProvider::APPLE);
        CHECK(credentials.provider_as_string() == IdentityProviderApple);
        CHECK(credentials.serialize_as_bson() ==
              bson::BsonDocument{{"provider", "oauth2-apple"}, {"id_token", "a_token"}});
    }

    SECTION("auth providers custom") {
        auto credentials = AppCredentials::custom("a_token");
        CHECK(credentials.provider() == AuthProvider::CUSTOM);
        CHECK(credentials.provider_as_string() == IdentityProviderCustom);
        CHECK(credentials.serialize_as_bson() ==
              bson::BsonDocument{{"provider", "custom-token"}, {"token", "a_token"}});
    }

    SECTION("auth providers username password") {
        auto credentials = AppCredentials::username_password("user", "pass");
        CHECK(credentials.provider() == AuthProvider::USERNAME_PASSWORD);
        CHECK(credentials.provider_as_string() == IdentityProviderUsernamePassword);
        CHECK(credentials.serialize_as_bson() ==
              bson::BsonDocument{{"provider", "local-userpass"}, {"username", "user"}, {"password", "pass"}});
    }

    SECTION("auth providers function") {
        bson::BsonDocument function_params{{"name", "mongo"}};
        auto credentials = AppCredentials::function(function_params);
        CHECK(credentials.provider() == AuthProvider::FUNCTION);
        CHECK(credentials.provider_as_string() == IdentityProviderFunction);
        CHECK(credentials.serialize_as_bson() == bson::BsonDocument{{"name", "mongo"}});
    }

    SECTION("auth providers api key") {
        auto credentials = AppCredentials::api_key("a key");
        CHECK(credentials.provider() == AuthProvider::API_KEY);
        CHECK(credentials.provider_as_string() == IdentityProviderAPIKey);
        CHECK(credentials.serialize_as_bson() == bson::BsonDocument{{"provider", "api-key"}, {"key", "a key"}});
        CHECK(enum_from_provider_type(provider_type_from_enum(AuthProvider::API_KEY)) == AuthProvider::API_KEY);
    }
}

TEST_CASE("app: refresh access token unit tests", "[sync][app][user][token]") {
    SECTION("refresh custom data happy path") {
        static bool session_route_hit = false;

        struct transport : UnitTestTransport {
            void send_request_to_server(const Request& request,
                                        util::UniqueFunction<void(const Response&)>&& completion) override
            {
                if (request.url.find("/session") != std::string::npos) {
                    session_route_hit = true;
                    nlohmann::json json{{"access_token", good_access_token}};
                    completion({200, 0, {}, json.dump()});
                }
                else {
                    UnitTestTransport::send_request_to_server(request, std::move(completion));
                }
            }
        };
        OfflineAppSession oas(OfflineAppSession::Config{instance_of<transport>});
        auto app = oas.app();
        oas.make_user();

        bool processed = false;
        app->refresh_custom_data(app->current_user(), [&](const Optional<AppError>& error) {
            REQUIRE_FALSE(error);
            CHECK(session_route_hit);
            processed = true;
        });
        CHECK(processed);
    }

    SECTION("refresh custom data sad path") {
        static bool session_route_hit = false;

        struct transport : UnitTestTransport {
            void send_request_to_server(const Request& request,
                                        util::UniqueFunction<void(const Response&)>&& completion) override
            {
                if (request.url.find("/session") != std::string::npos) {
                    session_route_hit = true;
                    nlohmann::json json{{"access_token", bad_access_token}};
                    completion({200, 0, {}, json.dump()});
                }
                else {
                    UnitTestTransport::send_request_to_server(request, std::move(completion));
                }
            }
        };

        OfflineAppSession oas(OfflineAppSession::Config{instance_of<transport>});
        auto app = oas.app();
        oas.make_user();

        bool processed = false;
        app->refresh_custom_data(app->current_user(), [&](const Optional<AppError>& error) {
            CHECK(error->reason() == "malformed JWT");
            CHECK(error->code() == ErrorCodes::BadToken);
            CHECK(session_route_hit);
            processed = true;
        });
        CHECK(processed);
    }

    SECTION("refresh token ensure flow is correct") {
        /*
         Expected flow:
         Login - this gets access and refresh tokens
         Get profile - throw back a 401 error
         Refresh token - get a new token for the user
         Get profile - get the profile with the new token
         */
        struct transport : GenericNetworkTransport {
            enum class TestState { unknown, location, login, profile_1, refresh, profile_2 };
            TestingStateMachine<TestState> state{TestState::unknown};
            void send_request_to_server(const Request& request,
                                        util::UniqueFunction<void(const Response&)>&& completion) override
            {
                if (request.url.find("/login") != std::string::npos) {
                    CHECK(state.get() == TestState::location);
                    state.transition_to(TestState::login);
                    completion({200, 0, {}, user_json(good_access_token).dump()});
                }
                else if (request.url.find("/profile") != std::string::npos) {
                    auto item = AppUtils::find_header("Authorization", request.headers);
                    CHECK(item);
                    auto access_token = item->second;
                    // simulated bad token request
                    if (access_token.find(good_access_token2) != std::string::npos) {
                        CHECK(state.get() == TestState::refresh);
                        state.transition_to(TestState::profile_2);
                        completion({200, 0, {}, user_profile_json().dump()});
                    }
                    else if (access_token.find(good_access_token) != std::string::npos) {
                        CHECK(state.get() == TestState::login);
                        state.transition_to(TestState::profile_1);
                        completion({401, 0, {}});
                    }
                }
                else if (request.url.find("/session") != std::string::npos && request.method == HttpMethod::post) {
                    CHECK(state.get() == TestState::profile_1);
                    state.transition_to(TestState::refresh);
                    nlohmann::json json{{"access_token", good_access_token2}};
                    completion({200, 0, {}, json.dump()});
                }
                else if (request.url.find("/location") != std::string::npos) {
                    CHECK(state.get() == TestState::unknown);
                    state.transition_to(TestState::location);
                    CHECK(request.method == HttpMethod::get);
                    completion({200,
                                0,
                                {},
                                "{\"deployment_model\":\"GLOBAL\",\"location\":\"US-VA\",\"hostname\":"
                                "\"http://localhost:9090\",\"ws_hostname\":\"ws://localhost:9090\"}"});
                }
                else {
                    FAIL("Unexpected request in test code" + request.url);
                }
            }
        };

        OfflineAppSession oas(OfflineAppSession::Config{instance_of<transport>});
        auto app = oas.app();
        REQUIRE(log_in(app));
    }
}

TEST_CASE("app: app released during async operation", "[app][user]") {
    struct Transport : public UnitTestTransport {
        std::string endpoint_to_hook;
        std::optional<Request> stored_request;
        util::UniqueFunction<void(const Response&)> stored_completion;

        void send_request_to_server(const Request& request,
                                    util::UniqueFunction<void(const Response&)>&& completion) override
        {
            // Store the completion handler for the chosen endpoint so that we can
            // invoke it after releasing the test's references to the App to
            // verify that it doesn't crash
            if (request.url.find(endpoint_to_hook) != std::string::npos) {
                REQUIRE_FALSE(stored_request);
                REQUIRE_FALSE(stored_completion);
                stored_request = request;
                stored_completion = std::move(completion);
                return;
            }

            UnitTestTransport::send_request_to_server(request, std::move(completion));
        }

        bool has_stored() const
        {
            return !!stored_completion;
        }

        void send_stored()
        {
            REQUIRE(stored_request);
            REQUIRE(stored_completion);
            UnitTestTransport::send_request_to_server(*stored_request, std::move(stored_completion));
            stored_request.reset();
            stored_completion = nullptr;
        }
    };
    auto transport = std::make_shared<Transport>();
    App::Config app_config;
    set_app_config_defaults(app_config, transport);
    SyncClientConfig sc_config;
    test_util::TestDirGuard base_path(util::make_temp_dir(), false);
    sc_config.base_file_path = base_path;
    sc_config.metadata_mode = SyncManager::MetadataMode::NoMetadata;

    SECTION("login") {
        transport->endpoint_to_hook = GENERATE("/location", "/login", "/profile");
        bool called = false;
        {
            auto app = App::get_app(App::CacheMode::Disabled, app_config, sc_config);
            app->log_in_with_credentials(AppCredentials::anonymous(),
                                         [&](std::shared_ptr<SyncUser> user, util::Optional<AppError> error) mutable {
                                             REQUIRE_FALSE(error);
                                             REQUIRE(user);
                                             REQUIRE(user->is_logged_in());
                                             called = true;
                                         });
            REQUIRE(transport->has_stored());
        }
        REQUIRE_FALSE(called);
        transport->send_stored();
        REQUIRE(called);
    }

    SECTION("access token refresh") {
        transport->endpoint_to_hook = "/auth/session";
        SECTION("directly via user") {
            bool completion_called = false;
            {
                auto app = App::get_app(App::CacheMode::Disabled, app_config, sc_config);
                create_user_and_log_in(app);
                app->current_user()->refresh_custom_data([&](std::optional<app::AppError> error) {
                    REQUIRE_FALSE(error);
                    completion_called = true;
                });
                REQUIRE(transport->has_stored());
            }

            REQUIRE_FALSE(completion_called);
            transport->send_stored();
            REQUIRE(completion_called);
        }

        SECTION("via sync session") {
            {
                auto app = App::get_app(App::CacheMode::Disabled, app_config, sc_config);
                create_user_and_log_in(app);
                auto user = app->current_user();
                SyncTestFile config(user, bson::Bson("test"));
                // give the user an expired access token so that the first use will try to refresh it
                user->update_access_token(encode_fake_jwt("token", 123, 456));
                REQUIRE_FALSE(transport->stored_completion);
                auto realm = Realm::get_shared_realm(config);
                REQUIRE(transport->has_stored());
            }
            transport->send_stored();
        }
    }

    REQUIRE_FALSE(transport->has_stored());
}

TEST_CASE("app: make_streaming_request", "[sync][app][streaming]") {
    UnitTestTransport::access_token = good_access_token;
    constexpr uint64_t timeout_ms = 60000; // this is the default
    OfflineAppSession oas({std::make_shared<UnitTestTransport>(timeout_ms)});
    auto app = oas.app();

    auto user = log_in(app);

    using Headers = decltype(Request().headers);

    const auto url_prefix = "https://some.fake.url/api/client/v2.0/app/app_id/functions/call?baas_request="sv;
    const auto get_request_args = [&](const Request& req) {
        REQUIRE(req.url.substr(0, url_prefix.size()) == url_prefix);
        auto args = req.url.substr(url_prefix.size());
        if (auto amp = args.find('&'); amp != std::string::npos) {
            args.resize(amp);
        }

        auto vec = util::base64_decode_to_vector(util::uri_percent_decode(args));
        REQUIRE(!!vec);
        auto parsed = bson::parse({vec->data(), vec->size()});
        REQUIRE(parsed.type() == bson::Bson::Type::Document);
        auto out = parsed.operator const bson::BsonDocument&();
        CHECK(out.size() == 3);
        return out;
    };

    const auto make_request = [&](std::shared_ptr<SyncUser> user, auto&&... args) {
        auto req = app->make_streaming_request(user, "func", bson::BsonArray{args...}, {"svc"});
        CHECK(req.method == HttpMethod::get);
        CHECK(req.body == "");
        CHECK(req.headers == Headers{{"Accept", "text/event-stream"}});
        CHECK(req.timeout_ms == timeout_ms);
        CHECK(req.uses_refresh_token == false);

        auto req_args = get_request_args(req);
        CHECK(req_args["name"] == "func");
        CHECK(req_args["service"] == "svc");
        CHECK(req_args["arguments"] == bson::BsonArray{args...});

        return req;
    };

    SECTION("no args") {
        auto req = make_request(nullptr);
        CHECK(req.url.find('&') == std::string::npos);
    }
    SECTION("args") {
        auto req = make_request(nullptr, "arg1", "arg2");
        CHECK(req.url.find('&') == std::string::npos);
    }
    SECTION("percent encoding") {
        // These force the base64 encoding to have + and / bytes and = padding, all of which are uri encoded.
        auto req = make_request(nullptr, ">>>>>?????");

        CHECK(req.url.find('&') == std::string::npos);
        CHECK(req.url.find("%2B") != std::string::npos);   // + (from >)
        CHECK(req.url.find("%2F") != std::string::npos);   // / (from ?)
        CHECK(req.url.find("%3D") != std::string::npos);   // = (tail padding)
        CHECK(req.url.rfind("%3D") == req.url.size() - 3); // = (tail padding)
    }
    SECTION("with user") {
        auto req = make_request(user, "arg1", "arg2");

        auto amp = req.url.find('&');
        REQUIRE(amp != std::string::npos);
        auto tail = req.url.substr(amp);
        REQUIRE(tail == ("&baas_at=" + user->access_token()));
    }
}

TEST_CASE("app: sync_user_profile unit tests", "[sync][app][user]") {
    SECTION("with empty map") {
        auto profile = SyncUserProfile(bson::BsonDocument());
        CHECK(profile.name() == util::none);
        CHECK(profile.email() == util::none);
        CHECK(profile.picture_url() == util::none);
        CHECK(profile.first_name() == util::none);
        CHECK(profile.last_name() == util::none);
        CHECK(profile.gender() == util::none);
        CHECK(profile.birthday() == util::none);
        CHECK(profile.min_age() == util::none);
        CHECK(profile.max_age() == util::none);
    }
    SECTION("with full map") {
        auto profile = SyncUserProfile(bson::BsonDocument({
            {"first_name", "Jan"},
            {"last_name", "Jaanson"},
            {"name", "Jan Jaanson"},
            {"email", "jan.jaanson@jaanson.com"},
            {"gender", "none"},
            {"birthday", "January 1, 1970"},
            {"min_age", "0"},
            {"max_age", "100"},
            {"picture_url", "some"},
        }));
        CHECK(profile.name() == "Jan Jaanson");
        CHECK(profile.email() == "jan.jaanson@jaanson.com");
        CHECK(profile.picture_url() == "some");
        CHECK(profile.first_name() == "Jan");
        CHECK(profile.last_name() == "Jaanson");
        CHECK(profile.gender() == "none");
        CHECK(profile.birthday() == "January 1, 1970");
        CHECK(profile.min_age() == "0");
        CHECK(profile.max_age() == "100");
    }
}

TEST_CASE("app: shared instances", "[sync][app]") {
    test_util::TestDirGuard test_dir(util::make_temp_dir(), false);

    App::Config base_config;
    set_app_config_defaults(base_config, instance_of<UnitTestTransport>);

    SyncClientConfig sync_config;
    sync_config.metadata_mode = SyncClientConfig::MetadataMode::NoMetadata;
    sync_config.base_file_path = test_dir;

    auto config1 = base_config;
    config1.app_id = "app1";

    auto config2 = base_config;
    config2.app_id = "app1";
    config2.base_url = "https://realm.mongodb.com"; // equivalent to default_base_url

    auto config3 = base_config;
    config3.app_id = "app2";

    auto config4 = base_config;
    config4.app_id = "app2";
    config4.base_url = "http://localhost:9090";

    // should all point to same underlying app
    auto app1_1 = App::get_app(app::App::CacheMode::Enabled, config1, sync_config);
    auto app1_2 = App::get_app(app::App::CacheMode::Enabled, config1, sync_config);
    auto app1_3 = App::get_cached_app(config1.app_id, config1.base_url);
    auto app1_4 = App::get_app(app::App::CacheMode::Enabled, config2, sync_config);
    auto app1_5 = App::get_cached_app(config1.app_id);

    CHECK(app1_1 == app1_2);
    CHECK(app1_1 == app1_3);
    CHECK(app1_1 == app1_4);
    CHECK(app1_1 == app1_5);

    // config3 and config4 should point to different apps
    auto app2_1 = App::get_app(app::App::CacheMode::Enabled, config3, sync_config);
    auto app2_2 = App::get_cached_app(config3.app_id, config3.base_url);
    auto app2_3 = App::get_app(app::App::CacheMode::Enabled, config4, sync_config);
    auto app2_4 = App::get_cached_app(config3.app_id);
    auto app2_5 = App::get_cached_app(config4.app_id, "https://some.different.url");

    CHECK(app2_1 == app2_2);
    CHECK(app2_1 != app2_3);
    CHECK(app2_4 != nullptr);
    CHECK(app2_5 == nullptr);

    CHECK(app1_1 != app2_1);
    CHECK(app1_1 != app2_3);
    CHECK(app1_1 != app2_4);
}
