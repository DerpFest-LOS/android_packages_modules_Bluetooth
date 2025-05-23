/*
 * Copyright 2019 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "common/id_generator.h"

#include <gtest/gtest.h>

TEST(IdGeneratorTest, sanity_test) {
  IdGenerator<5> generator;
  ASSERT_EQ(0, generator.GetNext());
  ASSERT_EQ(1, generator.GetNext());
  ASSERT_EQ(2, generator.GetNext());
  ASSERT_EQ(3, generator.GetNext());
  ASSERT_EQ(4, generator.GetNext());
  ASSERT_EQ(generator.ALL_USED, generator.GetNext());

  generator.Release(3);
  ASSERT_EQ(3, generator.GetNext());

  generator.Release(0);
  generator.Release(2);
  ASSERT_EQ(0, generator.GetNext());
  ASSERT_EQ(2, generator.GetNext());
}
