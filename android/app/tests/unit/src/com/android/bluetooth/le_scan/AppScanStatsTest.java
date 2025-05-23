/*
 * Copyright 2022 The Android Open Source Project
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

package com.android.bluetooth.le_scan;

import static com.android.bluetooth.Utils.sSystemClock;

import static com.google.common.truth.Truth.assertThat;

import static org.mockito.Mockito.mock;

import android.bluetooth.le.ScanFilter;
import android.bluetooth.le.ScanSettings;
import android.content.Context;
import android.os.BatteryStatsManager;
import android.os.WorkSource;

import androidx.test.filters.SmallTest;
import androidx.test.rule.ServiceTestRule;
import androidx.test.runner.AndroidJUnit4;

import com.android.bluetooth.TestUtils;
import com.android.bluetooth.btservice.AdapterService;
import com.android.internal.app.IBatteryStats;

import org.junit.Before;
import org.junit.Rule;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.mockito.Mock;
import org.mockito.junit.MockitoJUnit;
import org.mockito.junit.MockitoRule;

import java.util.ArrayList;
import java.util.List;

/** Test cases for {@link AppScanStats}. */
@SmallTest
@RunWith(AndroidJUnit4.class)
public class AppScanStatsTest {
    @Rule public final ServiceTestRule mServiceRule = new ServiceTestRule();
    @Rule public MockitoRule mockitoRule = MockitoJUnit.rule();

    @Mock private ScannerMap map;
    @Mock private TransitionalScanHelper mMockScanHelper;
    @Mock private AdapterService mAdapterService;

    // BatteryStatsManager is final and cannot be mocked with regular mockito, so just mock the
    // underlying binder calls.
    final BatteryStatsManager mBatteryStatsManager =
            new BatteryStatsManager(mock(IBatteryStats.class));

    @Before
    public void setUp() throws Exception {
        TestUtils.mockGetSystemService(
                mAdapterService,
                Context.BATTERY_STATS_SERVICE,
                BatteryStatsManager.class,
                mBatteryStatsManager);
    }

    @Test
    public void constructor() {
        String name = "appName";
        WorkSource source = null;

        AppScanStats appScanStats =
                new AppScanStats(name, source, map, mAdapterService, mMockScanHelper, sSystemClock);

        assertThat(appScanStats.mScannerMap).isEqualTo(map);
        assertThat(appScanStats.mScanHelper).isEqualTo(mMockScanHelper);

        assertThat(appScanStats.isScanning()).isEqualTo(false);
    }

    @Test
    public void testDump_doesNotCrash() throws Exception {
        String name = "appName";
        WorkSource source = null;

        AppScanStats appScanStats =
                new AppScanStats(name, source, map, mAdapterService, mMockScanHelper, sSystemClock);

        ScanSettings settings = new ScanSettings.Builder().build();
        List<ScanFilter> filters = new ArrayList<>();
        filters.add(new ScanFilter.Builder().setDeviceName("TestName").build());
        boolean isFilterScan = false;
        boolean isCallbackScan = false;
        int scannerId = 0;

        appScanStats.recordScanStart(
                settings, filters, isFilterScan, isCallbackScan, scannerId, "tag");
        appScanStats.isRegistered = true;

        StringBuilder stringBuilder = new StringBuilder();

        appScanStats.dumpToString(stringBuilder);
    }
}
