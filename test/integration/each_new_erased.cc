/*
 * Copyright (C) 2018 Open Source Robotics Foundation
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
 *
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

#include <ignition/common/Console.hh>
#include <sdf/World.hh>

#include "ignition/gazebo/Server.hh"
#include "ignition/gazebo/SystemLoader.hh"
#include "ignition/gazebo/test_config.hh"  // NOLINT(build/include)

#include "plugins/MockSystem.hh"

using namespace ignition;
using namespace std::chrono_literals;

class EachNewErasedFixture : public ::testing::Test
{
  protected: void SetUp() override
  {
    // Augment the system plugin path.  In SetUp to avoid test order issues.
    setenv("IGN_GAZEBO_SYSTEM_PLUGIN_PATH",
      (std::string(PROJECT_BINARY_PATH) + "/lib").c_str(), 1);
  }
};

class Relay
{
  public: Relay()
  {
    auto plugin = this->systemLoader.LoadPlugin(
        "libMockSystem.so", "ignition::gazebo::MockSystem", nullptr);
    EXPECT_TRUE(plugin.has_value());
    this->systemPtr = plugin.value();
    this->mockSystem = static_cast<gazebo::MockSystem *>(
        systemPtr->QueryInterface<gazebo::System>());
  }

  public: Relay &OnPreUpdate(gazebo::MockSystem::CallbackType cb)
  {
    this->mockSystem->preUpdateCallback = cb;
    return *this;
  }

  public: Relay &OnUpdate(gazebo::MockSystem::CallbackType cb)
  {
    this->mockSystem->updateCallback = cb;
    return *this;
  }

  public: Relay &OnPostUpdate(gazebo::MockSystem::CallbackTypeConst cb)
  {
    this->mockSystem->postUpdateCallback = cb;
    return *this;
  }

  public: ignition::gazebo::SystemPluginPtr systemPtr;

  private: gazebo::SystemLoader systemLoader;
  private: gazebo::MockSystem *mockSystem;
};

/////////////////////////////////////////////////
TEST_F(EachNewErasedFixture, EachNewEachErasedInSystem)
{
  ignition::gazebo::ServerConfig serverConfig;

  gazebo::Server server;

  server.SetUpdatePeriod(1ns);

  // Create entities on preupdate only once
  bool shouldCreateEntities{true};
  // Flag for erasing enties in the test system
  bool shouldEraseEntities{false};

  // Entities to be created in a system. These have to be out here so the
  // entityCreator can set the ids when it creates the entities and the
  // entityEraser system can access them easily
  gazebo::EntityId e1 = gazebo::kNullEntity;
  gazebo::EntityId e2 = gazebo::kNullEntity;

  Relay entityCreator;
  entityCreator.OnPreUpdate(
    [&](const gazebo::UpdateInfo &, gazebo::EntityComponentManager &_ecm)
    {
      if (shouldCreateEntities)
      {
        // Create entities only once
        e1 = _ecm.CreateEntity();
        e2 = _ecm.CreateEntity();
        _ecm.CreateComponent<int>(e1, 1);
        _ecm.CreateComponent<int>(e2, 2);
        shouldCreateEntities = false;
      }
  });

  Relay entityEraser;
  entityEraser.OnPreUpdate(
    [&](const gazebo::UpdateInfo &, gazebo::EntityComponentManager &_ecm)
    {
      if (shouldEraseEntities)
      {
        _ecm.RequestEraseEntity(e1);
        shouldEraseEntities = false;
      }
    });


  struct EntityCount
  {
    int newEntities = 0;
    int erasedEntities = 0;
  };

  EntityCount preCount;
  EntityCount updateCount;
  EntityCount postCount;

  auto counterFunc = [](EntityCount &_count)
  {
    // Lambda to return. This a simple counter that uses the appropriate count
    // variable where count = (pre, update, post)count
    auto counterImpl = [&](const gazebo::UpdateInfo &,
                           const gazebo::EntityComponentManager &_ecm)
    {
      _ecm.EachNew<int>([&](const gazebo::EntityId &, const int *) -> bool
      {
        ++_count.newEntities;
        return true;
      });
      _ecm.EachErased<int>([&](const gazebo::EntityId &, const int *) -> bool
      {
        ++_count.erasedEntities;
        return true;
      });
    };
    return counterImpl;
  };

  Relay entityCounter;
  entityCounter.OnPreUpdate(counterFunc(preCount));
  entityCounter.OnUpdate(counterFunc(updateCount));
  entityCounter.OnPostUpdate(counterFunc(postCount));

  server.AddSystem(entityCreator.systemPtr);
  server.AddSystem(entityEraser.systemPtr);
  server.AddSystem(entityCounter.systemPtr);

  EXPECT_FALSE(server.Running());
  server.Run(true, 1, false);

  // Assuming systems will run in the order they were inserted to the server,
  // the entityCounter system will see the new entities in the preupdate phase.
  EXPECT_EQ(2, preCount.newEntities);
  // The update and postupdate should see the new entities regardless of the
  // order of execution of systems
  EXPECT_EQ(2, updateCount.newEntities);
  EXPECT_EQ(2, postCount.newEntities);

  // Verify no erasures
  EXPECT_EQ(0, preCount.erasedEntities);
  EXPECT_EQ(0, updateCount.erasedEntities);
  EXPECT_EQ(0, postCount.erasedEntities);

  // reset counts
  preCount = EntityCount();
  updateCount = EntityCount();
  postCount = EntityCount();

  // This time, no new entities should be created
  server.Run(true, 1000, false);
  // After the second simulation step, the entities we created earlier are not
  // new anymore
  EXPECT_EQ(0, preCount.newEntities);
  EXPECT_EQ(0, updateCount.newEntities);
  EXPECT_EQ(0, postCount.newEntities);
  EXPECT_EQ(0, preCount.erasedEntities);
  EXPECT_EQ(0, updateCount.erasedEntities);
  EXPECT_EQ(0, postCount.erasedEntities);

  // reset counts
  preCount = EntityCount();
  updateCount = EntityCount();
  postCount = EntityCount();

  shouldEraseEntities = true;
  server.Run(true, 1, false);
  // Erase requested
  // Again, assuming systems will run in the order they were inserted to the
  // server, the entityCounter system will see the erased entities in the
  // preupdate phase.
  EXPECT_EQ(1, preCount.erasedEntities);
  // The update and postupdate should see the erased entities regardless of the
  // order of execution of systems
  EXPECT_EQ(1, updateCount.erasedEntities);
  EXPECT_EQ(1, postCount.erasedEntities);

  // reset counts
  preCount = EntityCount();
  updateCount = EntityCount();
  postCount = EntityCount();
  server.Run(true, 1, false);

  // Erased requests should be cleared after last simulation step
  EXPECT_EQ(0, preCount.erasedEntities);
  EXPECT_EQ(0, updateCount.erasedEntities);
  EXPECT_EQ(0, postCount.erasedEntities);
}