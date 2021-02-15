// Copyright (c) 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/webui/history/browsing_history_handler.h"

#include <stdint.h>
#include <memory>
#include <set>
#include <utility>

#include "base/bind.h"
#include "base/bind_helpers.h"
#include "base/macros.h"
#include "base/run_loop.h"
#include "base/strings/utf_string_conversions.h"
#include "base/test/simple_test_clock.h"
#include "base/values.h"
#include "build/build_config.h"
#include "chrome/browser/history/web_history_service_factory.h"
#include "chrome/browser/sync/profile_sync_service_factory.h"
#include "chrome/test/base/chrome_render_view_host_test_harness.h"
#include "chrome/test/base/testing_profile.h"
#include "components/history/core/browser/browsing_history_service.h"
#include "components/history/core/test/fake_web_history_service.h"
#include "components/sync/base/model_type.h"
#include "components/sync/driver/test_sync_service.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/test_web_ui.h"
#include "net/http/http_status_code.h"
#include "net/traffic_annotation/network_traffic_annotation_test_helper.h"
#include "url/gurl.h"

namespace {

base::Time PretendNow() {
  base::Time::Exploded exploded_reference_time;
  exploded_reference_time.year = 2015;
  exploded_reference_time.month = 1;
  exploded_reference_time.day_of_month = 2;
  exploded_reference_time.day_of_week = 5;
  exploded_reference_time.hour = 11;
  exploded_reference_time.minute = 0;
  exploded_reference_time.second = 0;
  exploded_reference_time.millisecond = 0;

  base::Time out_time;
  EXPECT_TRUE(
      base::Time::FromLocalExploded(exploded_reference_time, &out_time));
  return out_time;
}

class BrowsingHistoryHandlerWithWebUIForTesting
    : public BrowsingHistoryHandler {
 public:
  explicit BrowsingHistoryHandlerWithWebUIForTesting(content::WebUI* web_ui) {
    set_clock(&test_clock_);
    set_web_ui(web_ui);
    test_clock_.SetNow(PretendNow());
  }

  base::SimpleTestClock* test_clock() { return &test_clock_; }

 private:
  base::SimpleTestClock test_clock_;

  DISALLOW_COPY_AND_ASSIGN(BrowsingHistoryHandlerWithWebUIForTesting);
};

}  // namespace

class BrowsingHistoryHandlerTest : public ChromeRenderViewHostTestHarness {
 public:
  void SetUp() override {
    ChromeRenderViewHostTestHarness::SetUp();
    profile()->CreateBookmarkModel(false);

    sync_service_ = static_cast<syncer::TestSyncService*>(
        ProfileSyncServiceFactory::GetForProfile(profile()));
    web_history_service_ = static_cast<history::FakeWebHistoryService*>(
        WebHistoryServiceFactory::GetForProfile(profile()));
    ASSERT_TRUE(web_history_service_);

    web_ui_ = std::make_unique<content::TestWebUI>();
    web_ui_->set_web_contents(web_contents());
  }

  void TearDown() override {
    web_ui_.reset();
    ChromeRenderViewHostTestHarness::TearDown();
  }

  TestingProfile::TestingFactories GetTestingFactories() const override {
    return {
        {ProfileSyncServiceFactory::GetInstance(),
         base::BindRepeating(&BuildTestSyncService)},
        {WebHistoryServiceFactory::GetInstance(),
         base::BindRepeating(&BuildFakeWebHistoryService)},
    };
  }

  void VerifyHistoryDeletedFired() {
    const content::TestWebUI::CallData& data = *web_ui()->call_data().back();
    EXPECT_EQ("cr.webUIListenerCallback", data.function_name());
    std::string event_fired;
    ASSERT_TRUE(data.arg1()->GetAsString(&event_fired));
    EXPECT_EQ("history-deleted", event_fired);
  }

  void InitializeWebUI(BrowsingHistoryHandlerWithWebUIForTesting& handler) {
    // Send historyLoaded so that JS will be allowed.
    base::Value init_args(base::Value::Type::LIST);
    init_args.Append("history-loaded-callback-id");
    handler.HandleHistoryLoaded(&base::Value::AsListValue(init_args));
  }

  syncer::TestSyncService* sync_service() { return sync_service_; }
  history::WebHistoryService* web_history_service() {
    return web_history_service_;
  }
  content::TestWebUI* web_ui() { return web_ui_.get(); }

 private:
  static std::unique_ptr<KeyedService> BuildTestSyncService(
      content::BrowserContext* context) {
    return std::make_unique<syncer::TestSyncService>();
  }

  static std::unique_ptr<KeyedService> BuildFakeWebHistoryService(
      content::BrowserContext* context) {
    std::unique_ptr<history::FakeWebHistoryService> service =
        std::make_unique<history::FakeWebHistoryService>();
    service->SetupFakeResponse(true /* success */, net::HTTP_OK);
    return service;
  }

  syncer::TestSyncService* sync_service_ = nullptr;
  history::FakeWebHistoryService* web_history_service_ = nullptr;
  std::unique_ptr<content::TestWebUI> web_ui_;
};

// Tests that BrowsingHistoryHandler is informed about WebHistoryService
// deletions.
TEST_F(BrowsingHistoryHandlerTest, ObservingWebHistoryDeletions) {
  base::Callback<void(bool)> callback = base::DoNothing();

  // BrowsingHistoryHandler is informed about WebHistoryService history
  // deletions.
  {
    sync_service()->SetTransportState(
        syncer::SyncService::TransportState::ACTIVE);
    BrowsingHistoryHandlerWithWebUIForTesting handler(web_ui());
    handler.RegisterMessages();
    InitializeWebUI(handler);

    web_history_service()->ExpireHistoryBetween(
        std::set<GURL>(), base::Time(), base::Time::Max(), callback,
        PARTIAL_TRAFFIC_ANNOTATION_FOR_TESTS);

    EXPECT_EQ(1U, web_ui()->call_data().size());
    VerifyHistoryDeletedFired();
  }

  // BrowsingHistoryHandler will be informed about WebHistoryService deletions
  // even if history sync is activated later.
  {
    sync_service()->SetTransportState(
        syncer::SyncService::TransportState::INITIALIZING);
    BrowsingHistoryHandlerWithWebUIForTesting handler(web_ui());
    handler.RegisterMessages();
    sync_service()->SetTransportState(
        syncer::SyncService::TransportState::ACTIVE);
    sync_service()->FireStateChanged();

    web_history_service()->ExpireHistoryBetween(
        std::set<GURL>(), base::Time(), base::Time::Max(), callback,
        PARTIAL_TRAFFIC_ANNOTATION_FOR_TESTS);

    // Simulate initialization after history has been deleted.
    InitializeWebUI(handler);

    EXPECT_EQ(2U, web_ui()->call_data().size());
    VerifyHistoryDeletedFired();
  }

  // BrowsingHistoryHandler does not fire historyDeleted while a web history
  // delete request is happening.
  {
    sync_service()->SetTransportState(
        syncer::SyncService::TransportState::ACTIVE);
    BrowsingHistoryHandlerWithWebUIForTesting handler(web_ui());
    handler.RegisterMessages();
    InitializeWebUI(handler);

    // Simulate a delete request.
    base::Value args(base::Value::Type::LIST);
    args.Append("remove-visits-callback-id");
    base::Value to_remove(base::Value::Type::LIST);
    base::Value visit(base::Value::Type::DICTIONARY);
    visit.SetStringKey("url", "https://www.google.com");
    base::Value timestamps(base::Value::Type::LIST);
    timestamps.Append(12345678.0);
    visit.SetKey("timestamps", std::move(timestamps));
    to_remove.Append(std::move(visit));
    args.Append(std::move(to_remove));
    handler.HandleRemoveVisits(&base::Value::AsListValue(args));

    EXPECT_EQ(3U, web_ui()->call_data().size());
    const content::TestWebUI::CallData& data = *web_ui()->call_data().back();
    EXPECT_EQ("cr.webUIResponse", data.function_name());
    std::string callback_id;
    ASSERT_TRUE(data.arg1()->GetAsString(&callback_id));
    EXPECT_EQ("remove-visits-callback-id", callback_id);
    bool success = false;
    ASSERT_TRUE(data.arg2()->GetAsBoolean(&success));
    ASSERT_TRUE(success);
  }

  // When history sync is not active, we don't listen to WebHistoryService
  // deletions. The WebHistoryService object still exists (because it's a
  // BrowserContextKeyedService), but is not visible to BrowsingHistoryHandler.
  {
    sync_service()->SetTransportState(
        syncer::SyncService::TransportState::INITIALIZING);
    BrowsingHistoryHandlerWithWebUIForTesting handler(web_ui());
    handler.RegisterMessages();

    web_history_service()->ExpireHistoryBetween(
        std::set<GURL>(), base::Time(), base::Time::Max(), callback,
        PARTIAL_TRAFFIC_ANNOTATION_FOR_TESTS);

    // No additional WebUI calls were made.
    EXPECT_EQ(3U, web_ui()->call_data().size());
  }
}

#if !defined(OS_ANDROID)
TEST_F(BrowsingHistoryHandlerTest, MdTruncatesTitles) {
  history::BrowsingHistoryService::HistoryEntry long_url_entry;
  long_url_entry.url = GURL(
      "http://loooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooo"
      "oooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooo"
      "oooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooo"
      "oooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooo"
      "oooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooo"
      "oooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooo"
      "ngurlislong.com");
  ASSERT_GT(long_url_entry.url.spec().size(), 300U);

  BrowsingHistoryHandlerWithWebUIForTesting handler(web_ui());
  ASSERT_TRUE(web_ui()->call_data().empty());

  handler.OnQueryComplete({long_url_entry},
                          history::BrowsingHistoryService::QueryResultsInfo(),
                          base::OnceClosure());
  ASSERT_FALSE(web_ui()->call_data().empty());

  const base::ListValue* arg2;
  ASSERT_TRUE(web_ui()->call_data().front()->arg2()->GetAsList(&arg2));

  const base::DictionaryValue* first_entry;
  ASSERT_TRUE(arg2->GetDictionary(0, &first_entry));

  base::string16 title;
  ASSERT_TRUE(first_entry->GetString("title", &title));

  ASSERT_EQ(0u, title.find(base::ASCIIToUTF16("http://loooo")));
  EXPECT_EQ(300u, title.size());
}
#endif