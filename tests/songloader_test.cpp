/* This file is part of Clementine.

   Clementine is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   Clementine is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with Clementine.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "test_utils.h"
#include "gmock/gmock-matchers.h"
#include "gtest/gtest.h"

#include "core/songloader.h"
#include "engines/gstengine.h"

#include <QBuffer>
#include <QEventLoop>
#include <QSignalSpy>

#include <boost/scoped_ptr.hpp>

class SongLoaderTest : public ::testing::Test {
public:
 static void SetUpTestCase() {
   sGstEngine = new GstEngine;
   ASSERT_TRUE(sGstEngine->Init());
 }

 static void TearDownTestCase() {
   delete sGstEngine;
   sGstEngine = NULL;
 }

protected:
  void SetUp() {
    loader_.reset(new SongLoader);
  }

  static const char* kRemoteUrl;
  static GstEngine* sGstEngine;

  boost::scoped_ptr<SongLoader> loader_;
};

const char* SongLoaderTest::kRemoteUrl = "http://remotetestdata.clementine-player.org";
GstEngine* SongLoaderTest::sGstEngine = NULL;

TEST_F(SongLoaderTest, LoadLocalMp3) {
  TemporaryResource file(":/testdata/beep.mp3");
  SongLoader::Result ret = loader_->Load(QUrl::fromLocalFile(file.fileName()));

  ASSERT_EQ(SongLoader::Success, ret);
  ASSERT_EQ(1, loader_->songs().count());
  EXPECT_TRUE(loader_->songs()[0].is_valid());
  EXPECT_EQ("Beep mp3", loader_->songs()[0].title());
}

TEST_F(SongLoaderTest, LoadLocalPls) {
  TemporaryResource file(":/testdata/pls_one.pls");
  SongLoader::Result ret = loader_->Load(QUrl::fromLocalFile(file.fileName()));

  ASSERT_EQ(SongLoader::Success, ret);
  ASSERT_EQ(1, loader_->songs().count());
  EXPECT_EQ("Title", loader_->songs()[0].title());
  EXPECT_EQ(123, loader_->songs()[0].length());
}

TEST_F(SongLoaderTest, LoadLocalM3U) {
  TemporaryResource file(":/testdata/test.m3u");
  SongLoader::Result ret = loader_->Load(QUrl::fromLocalFile(file.fileName()));

  ASSERT_EQ(SongLoader::Success, ret);
  ASSERT_EQ(239, loader_->songs().count());
}

TEST_F(SongLoaderTest, LoadLocalXSPF) {
  TemporaryResource file(":/testdata/test.xspf");
  SongLoader::Result ret = loader_->Load(QUrl::fromLocalFile(file.fileName()));

  ASSERT_EQ(SongLoader::Success, ret);
  ASSERT_EQ(1, loader_->songs().count());
  EXPECT_EQ("Foo", loader_->songs()[0].title());
}

TEST_F(SongLoaderTest, LoadLocalASX) {
  TemporaryResource file(":/testdata/test.asx");
  SongLoader::Result ret = loader_->Load(QUrl::fromLocalFile(file.fileName()));

  ASSERT_EQ(SongLoader::Success, ret);
  ASSERT_EQ(1, loader_->songs().count());
  EXPECT_EQ("Foo", loader_->songs()[0].title());
}

TEST_F(SongLoaderTest, LoadRemoteMp3) {
  SongLoader::Result ret = loader_->Load(QString(kRemoteUrl) + "/beep.mp3");
  ASSERT_EQ(SongLoader::WillLoadAsync, ret);

  QSignalSpy spy(loader_.get(), SIGNAL(LoadFinished(bool)));

  // Start an event loop to wait for gstreamer to do its thing
  QEventLoop loop;
  QObject::connect(loader_.get(), SIGNAL(LoadFinished(bool)),
                   &loop, SLOT(quit()));
  loop.exec();

  // Check the signal was emitted with Success
  ASSERT_EQ(1, spy.count());
  EXPECT_EQ(true, spy[0][0].toBool());

  // Check the song got loaded
  ASSERT_EQ(1, loader_->songs().count());
}

TEST_F(SongLoaderTest, LoadRemote404) {
  SongLoader::Result ret = loader_->Load(QString(kRemoteUrl) + "/404.mp3");
  ASSERT_EQ(SongLoader::WillLoadAsync, ret);

  QSignalSpy spy(loader_.get(), SIGNAL(LoadFinished(bool)));

  // Start an event loop to wait for gstreamer to do its thing
  QEventLoop loop;
  QObject::connect(loader_.get(), SIGNAL(LoadFinished(bool)),
                   &loop, SLOT(quit()));
  loop.exec();

  // Check the signal was emitted with Error
  ASSERT_EQ(1, spy.count());
  EXPECT_EQ(false, spy[0][0].toBool());
}

TEST_F(SongLoaderTest, LoadRemotePls) {
  SongLoader::Result ret = loader_->Load(QString(kRemoteUrl) + "/pls_somafm.pls");
  ASSERT_EQ(SongLoader::WillLoadAsync, ret);

  QSignalSpy spy(loader_.get(), SIGNAL(LoadFinished(bool)));

  // Start an event loop to wait for gstreamer to do its thing
  QEventLoop loop;
  QObject::connect(loader_.get(), SIGNAL(LoadFinished(bool)),
                   &loop, SLOT(quit()));
  loop.exec();

  // Check the signal was emitted with Success
  ASSERT_EQ(1, spy.count());
  EXPECT_EQ(true, spy[0][0].toBool());

  // Check some metadata
  ASSERT_EQ(4, loader_->songs().count());
  EXPECT_EQ("SomaFM: Groove Salad (#3 128k mp3): A nicely chilled plate of ambient beats and grooves.",
            loader_->songs()[2].title());
  EXPECT_EQ("http://ice.somafm.com/groovesalad", loader_->songs()[3].filename());
}

TEST_F(SongLoaderTest, LoadRemotePlainText) {
  SongLoader::Result ret = loader_->Load(QString(kRemoteUrl) + "/notaplaylist.txt");
  ASSERT_EQ(SongLoader::WillLoadAsync, ret);

  QSignalSpy spy(loader_.get(), SIGNAL(LoadFinished(bool)));

  // Start an event loop to wait for gstreamer to do its thing
  QEventLoop loop;
  QObject::connect(loader_.get(), SIGNAL(LoadFinished(bool)),
                   &loop, SLOT(quit()));
  loop.exec();

  // Check the signal was emitted with Error
  ASSERT_EQ(1, spy.count());
  EXPECT_EQ(false, spy[0][0].toBool());
}