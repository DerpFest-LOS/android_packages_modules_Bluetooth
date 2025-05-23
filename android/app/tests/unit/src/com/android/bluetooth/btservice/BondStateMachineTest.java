/*
 * Copyright 2018 The Android Open Source Project
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
package com.android.bluetooth.btservice;

import static android.Manifest.permission.BLUETOOTH_CONNECT;

import static org.mockito.Mockito.*;

import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothManager;
import android.content.Context;
import android.content.Intent;
import android.os.Bundle;
import android.os.HandlerThread;
import android.os.Message;
import android.os.ParcelUuid;
import android.os.UserHandle;

import androidx.test.InstrumentationRegistry;
import androidx.test.filters.MediumTest;
import androidx.test.runner.AndroidJUnit4;

import com.android.bluetooth.TestUtils;
import com.android.bluetooth.Utils;

import org.junit.After;
import org.junit.Assert;
import org.junit.Before;
import org.junit.Rule;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.mockito.ArgumentCaptor;
import org.mockito.Mock;
import org.mockito.junit.MockitoJUnit;
import org.mockito.junit.MockitoRule;

@MediumTest
@RunWith(AndroidJUnit4.class)
public class BondStateMachineTest {
    private static final int TEST_BOND_REASON = 0;
    private static final byte[] TEST_BT_ADDR_BYTES = {00, 11, 22, 33, 44, 55};
    private static final byte[] TEST_BT_ADDR_BYTES_2 = {00, 11, 22, 33, 44, 66};
    private static final int[] DEVICE_TYPES = {
        BluetoothDevice.DEVICE_TYPE_CLASSIC,
        BluetoothDevice.DEVICE_TYPE_DUAL,
        BluetoothDevice.DEVICE_TYPE_LE
    };
    private static final ParcelUuid[] TEST_UUIDS = {
        ParcelUuid.fromString("0000111E-0000-1000-8000-00805F9B34FB")
    };

    private static final int BOND_NONE = BluetoothDevice.BOND_NONE;
    private static final int BOND_BONDING = BluetoothDevice.BOND_BONDING;
    private static final int BOND_BONDED = BluetoothDevice.BOND_BONDED;

    private BluetoothManager mBluetoothManager;
    private AdapterProperties mAdapterProperties;
    private BluetoothDevice mDevice;
    private Context mTargetContext;
    private RemoteDevices mRemoteDevices;
    private BondStateMachine mBondStateMachine;
    private HandlerThread mHandlerThread;
    private RemoteDevices.DeviceProperties mDeviceProperties;
    private int mVerifyCount = 0;

    @Rule public MockitoRule mockitoRule = MockitoJUnit.rule();

    @Mock private AdapterService mAdapterService;
    @Mock private AdapterNativeInterface mNativeInterface;

    @Before
    public void setUp() throws Exception {
        mTargetContext = InstrumentationRegistry.getTargetContext();
        TestUtils.setAdapterService(mAdapterService);
        doReturn(mNativeInterface).when(mAdapterService).getNative();
        mHandlerThread = new HandlerThread("BondStateMachineTestHandlerThread");
        mHandlerThread.start();

        mBluetoothManager = mTargetContext.getSystemService(BluetoothManager.class);
        when(mAdapterService.getSystemService(Context.BLUETOOTH_SERVICE))
                .thenReturn(mBluetoothManager);
        when(mAdapterService.getSystemServiceName(BluetoothManager.class))
                .thenReturn(Context.BLUETOOTH_SERVICE);

        mRemoteDevices = new RemoteDevices(mAdapterService, mHandlerThread.getLooper());
        mRemoteDevices.reset();
        when(mAdapterService.getResources()).thenReturn(mTargetContext.getResources());
        mAdapterProperties =
                new AdapterProperties(mAdapterService, mRemoteDevices, mHandlerThread.getLooper());
        mAdapterProperties.init();
        mBondStateMachine =
                BondStateMachine.make(mAdapterService, mAdapterProperties, mRemoteDevices);
    }

    @After
    public void tearDown() throws Exception {
        mHandlerThread.quit();
        TestUtils.clearAdapterService(mAdapterService);
    }

    @Test
    public void testCreateBondAfterRemoveBond() {
        // Set up two devices already bonded.
        mRemoteDevices.reset();
        RemoteDevices.DeviceProperties deviceProperties1, deviceProperties2;
        deviceProperties1 = mRemoteDevices.addDeviceProperties(TEST_BT_ADDR_BYTES);
        deviceProperties2 = mRemoteDevices.addDeviceProperties(TEST_BT_ADDR_BYTES_2);
        BluetoothDevice device1, device2;
        device1 = mRemoteDevices.getDevice(TEST_BT_ADDR_BYTES);
        device2 = mRemoteDevices.getDevice(TEST_BT_ADDR_BYTES_2);
        deviceProperties1.mBondState = BOND_BONDED;
        deviceProperties2.mBondState = BOND_BONDED;

        doReturn(true).when(mNativeInterface).removeBond(any(byte[].class));
        doReturn(true)
                .when(mNativeInterface)
                .createBond(any(byte[].class), eq(BluetoothDevice.ADDRESS_TYPE_PUBLIC), anyInt());

        // The removeBond() request for a bonded device should invoke the removeBondNative() call.
        Message removeBondMsg1 = mBondStateMachine.obtainMessage(BondStateMachine.REMOVE_BOND);
        removeBondMsg1.obj = device1;
        mBondStateMachine.sendMessage(removeBondMsg1);
        TestUtils.waitForLooperToFinishScheduledTask(mBondStateMachine.getHandler().getLooper());
        Message removeBondMsg2 = mBondStateMachine.obtainMessage(BondStateMachine.REMOVE_BOND);
        removeBondMsg2.obj = device2;
        mBondStateMachine.sendMessage(removeBondMsg2);
        TestUtils.waitForLooperToFinishScheduledTask(mBondStateMachine.getHandler().getLooper());

        verify(mNativeInterface).removeBond(eq(TEST_BT_ADDR_BYTES));
        verify(mNativeInterface).removeBond(eq(TEST_BT_ADDR_BYTES_2));

        mBondStateMachine.bondStateChangeCallback(
                AbstractionLayer.BT_STATUS_SUCCESS, TEST_BT_ADDR_BYTES, BOND_NONE, 0);
        TestUtils.waitForLooperToFinishScheduledTask(mBondStateMachine.getHandler().getLooper());
        mBondStateMachine.bondStateChangeCallback(
                AbstractionLayer.BT_STATUS_SUCCESS, TEST_BT_ADDR_BYTES_2, BOND_NONE, 0);
        TestUtils.waitForLooperToFinishScheduledTask(mBondStateMachine.getHandler().getLooper());

        // Try to pair these two devices again, createBondNative() should be invoked.
        Message createBondMsg1 = mBondStateMachine.obtainMessage(BondStateMachine.CREATE_BOND);
        createBondMsg1.obj = device1;
        mBondStateMachine.sendMessage(createBondMsg1);
        Message createBondMsg2 = mBondStateMachine.obtainMessage(BondStateMachine.CREATE_BOND);
        createBondMsg2.obj = device2;
        mBondStateMachine.sendMessage(createBondMsg2);
        TestUtils.waitForLooperToFinishScheduledTask(mBondStateMachine.getHandler().getLooper());

        verify(mNativeInterface)
                .createBond(
                        eq(TEST_BT_ADDR_BYTES), eq(BluetoothDevice.ADDRESS_TYPE_PUBLIC), anyInt());
        verify(mNativeInterface)
                .createBond(
                        eq(TEST_BT_ADDR_BYTES_2),
                        eq(BluetoothDevice.ADDRESS_TYPE_PUBLIC),
                        anyInt());
    }

    @Test
    public void testCreateBondWithLeDevice() {
        mRemoteDevices.reset();
        mBondStateMachine.mPendingBondedDevices.clear();

        BluetoothDevice device1 =
                BluetoothAdapter.getDefaultAdapter()
                        .getRemoteLeDevice(
                                Utils.getAddressStringFromByte(TEST_BT_ADDR_BYTES),
                                BluetoothDevice.ADDRESS_TYPE_PUBLIC);
        BluetoothDevice device2 =
                BluetoothAdapter.getDefaultAdapter()
                        .getRemoteLeDevice(
                                Utils.getAddressStringFromByte(TEST_BT_ADDR_BYTES_2),
                                BluetoothDevice.ADDRESS_TYPE_RANDOM);

        // The createBond() request for two devices with different address types.
        Message createBondMsg1 = mBondStateMachine.obtainMessage(BondStateMachine.CREATE_BOND);
        createBondMsg1.obj = device1;
        mBondStateMachine.sendMessage(createBondMsg1);
        Message createBondMsg2 = mBondStateMachine.obtainMessage(BondStateMachine.CREATE_BOND);
        createBondMsg2.obj = device2;
        mBondStateMachine.sendMessage(createBondMsg2);
        TestUtils.waitForLooperToFinishScheduledTask(mBondStateMachine.getHandler().getLooper());

        verify(mNativeInterface)
                .createBond(
                        eq(TEST_BT_ADDR_BYTES), eq(BluetoothDevice.ADDRESS_TYPE_PUBLIC), anyInt());
        verify(mNativeInterface)
                .createBond(
                        eq(TEST_BT_ADDR_BYTES_2),
                        eq(BluetoothDevice.ADDRESS_TYPE_RANDOM),
                        anyInt());
    }

    @Test
    public void testUuidUpdateWithPendingDevice() {
        mRemoteDevices.reset();
        mBondStateMachine.mPendingBondedDevices.clear();

        RemoteDevices.DeviceProperties pendingDeviceProperties =
                mRemoteDevices.addDeviceProperties(TEST_BT_ADDR_BYTES_2);
        BluetoothDevice pendingDevice = pendingDeviceProperties.getDevice();
        Assert.assertNotNull(pendingDevice);
        mBondStateMachine.sendIntent(pendingDevice, BOND_BONDED, TEST_BOND_REASON, false);

        RemoteDevices.DeviceProperties testDeviceProperties =
                mRemoteDevices.addDeviceProperties(TEST_BT_ADDR_BYTES);
        testDeviceProperties.mUuids = TEST_UUIDS;
        BluetoothDevice testDevice = testDeviceProperties.getDevice();
        Assert.assertNotNull(testDevice);

        Message bondingMsg = mBondStateMachine.obtainMessage(BondStateMachine.BONDING_STATE_CHANGE);
        bondingMsg.obj = testDevice;
        bondingMsg.arg1 = BOND_BONDING;
        bondingMsg.arg2 = AbstractionLayer.BT_STATUS_RMT_DEV_DOWN;
        mBondStateMachine.sendMessage(bondingMsg);

        pendingDeviceProperties.mUuids = TEST_UUIDS;
        Message uuidUpdateMsg = mBondStateMachine.obtainMessage(BondStateMachine.UUID_UPDATE);
        uuidUpdateMsg.obj = pendingDevice;

        mBondStateMachine.sendMessage(uuidUpdateMsg);

        Message bondedMsg = mBondStateMachine.obtainMessage(BondStateMachine.BONDING_STATE_CHANGE);
        bondedMsg.obj = testDevice;
        bondedMsg.arg1 = BOND_BONDED;
        bondedMsg.arg2 = AbstractionLayer.BT_STATUS_SUCCESS;
        mBondStateMachine.sendMessage(bondedMsg);

        TestUtils.waitForLooperToFinishScheduledTask(mBondStateMachine.getHandler().getLooper());
        Assert.assertTrue(mBondStateMachine.mPendingBondedDevices.isEmpty());
    }

    private void resetRemoteDevice(int deviceType) {
        // Reset mRemoteDevices for the test.
        mRemoteDevices.reset();
        mDeviceProperties = mRemoteDevices.addDeviceProperties(TEST_BT_ADDR_BYTES);
        mDevice = mDeviceProperties.getDevice();
        Assert.assertNotNull(mDevice);
        mDeviceProperties.mDeviceType = deviceType;
        mBondStateMachine.mPendingBondedDevices.clear();
    }

    @Test
    public void testSendIntent() {
        int badBondState = 42;
        mVerifyCount = 0;

        // Uuid not available, mPendingBondedDevice is empty.
        testSendIntentNoPendingDevice(
                BOND_NONE, BOND_NONE, false, BOND_NONE, false, BOND_NONE, BOND_NONE, false);

        testSendIntentNoPendingDevice(
                BOND_NONE, BOND_BONDING, false, BOND_BONDING, true, BOND_NONE, BOND_BONDING, false);
        testSendIntentNoPendingDevice(
                BOND_NONE, BOND_BONDED, false, BOND_BONDED, true, BOND_NONE, BOND_BONDING, true);
        testSendIntentNoPendingDevice(
                BOND_NONE, badBondState, false, BOND_NONE, false, BOND_NONE, BOND_NONE, false);
        testSendIntentNoPendingDevice(
                BOND_BONDING, BOND_NONE, false, BOND_NONE, true, BOND_BONDING, BOND_NONE, false);
        testSendIntentNoPendingDevice(
                BOND_BONDING,
                BOND_BONDING,
                false,
                BOND_BONDING,
                false,
                BOND_NONE,
                BOND_NONE,
                false);
        testSendIntentNoPendingDevice(
                BOND_BONDING, BOND_BONDED, false, BOND_BONDED, false, BOND_NONE, BOND_NONE, true);
        testSendIntentNoPendingDevice(
                BOND_BONDING,
                badBondState,
                false,
                BOND_BONDING,
                false,
                BOND_NONE,
                BOND_NONE,
                false);
        testSendIntentNoPendingDevice(
                BOND_BONDED, BOND_NONE, false, BOND_NONE, true, BOND_BONDED, BOND_NONE, false);
        testSendIntentNoPendingDevice(
                BOND_BONDED,
                BOND_BONDING,
                false,
                BOND_BONDING,
                true,
                BOND_BONDED,
                BOND_BONDING,
                false);
        testSendIntentNoPendingDevice(
                BOND_BONDED, BOND_BONDED, false, BOND_BONDED, false, BOND_NONE, BOND_NONE, false);
        testSendIntentNoPendingDevice(
                BOND_BONDED, badBondState, false, BOND_BONDED, false, BOND_NONE, BOND_NONE, false);

        testSendIntentNoPendingDevice(
                BOND_NONE, BOND_NONE, true, BOND_NONE, false, BOND_NONE, BOND_NONE, false);
        testSendIntentNoPendingDevice(
                BOND_NONE, BOND_BONDING, true, BOND_NONE, false, BOND_NONE, BOND_NONE, false);
        testSendIntentNoPendingDevice(
                BOND_NONE, BOND_BONDED, true, BOND_NONE, false, BOND_NONE, BOND_NONE, false);
        testSendIntentNoPendingDevice(
                BOND_NONE, badBondState, true, BOND_NONE, false, BOND_NONE, BOND_NONE, false);
        testSendIntentNoPendingDevice(
                BOND_BONDING, BOND_NONE, true, BOND_BONDING, false, BOND_NONE, BOND_NONE, false);
        testSendIntentNoPendingDevice(
                BOND_BONDING, BOND_BONDING, true, BOND_BONDING, false, BOND_NONE, BOND_NONE, false);
        testSendIntentNoPendingDevice(
                BOND_BONDING, BOND_BONDED, true, BOND_BONDING, false, BOND_NONE, BOND_NONE, false);
        testSendIntentNoPendingDevice(
                BOND_BONDING, badBondState, true, BOND_BONDING, false, BOND_NONE, BOND_NONE, false);
        testSendIntentNoPendingDevice(
                BOND_BONDED, BOND_NONE, true, BOND_BONDED, false, BOND_NONE, BOND_NONE, false);
        testSendIntentNoPendingDevice(
                BOND_BONDED, BOND_BONDING, true, BOND_BONDED, false, BOND_NONE, BOND_NONE, false);
        testSendIntentNoPendingDevice(
                BOND_BONDED, BOND_BONDED, true, BOND_BONDED, false, BOND_NONE, BOND_NONE, false);
        testSendIntentNoPendingDevice(
                BOND_BONDED, badBondState, true, BOND_BONDED, false, BOND_NONE, BOND_NONE, false);

        // Uuid not available, mPendingBondedDevice contains a remote device.
        testSendIntentPendingDevice(
                BOND_NONE, BOND_NONE, false, BOND_NONE, false, BOND_NONE, BOND_NONE, false);
        testSendIntentPendingDevice(
                BOND_NONE, BOND_BONDING, false, BOND_NONE, false, BOND_NONE, BOND_NONE, false);
        testSendIntentPendingDevice(
                BOND_NONE, BOND_BONDED, false, BOND_NONE, false, BOND_NONE, BOND_NONE, false);
        testSendIntentPendingDevice(
                BOND_NONE, badBondState, false, BOND_NONE, false, BOND_NONE, BOND_NONE, false);
        testSendIntentPendingDevice(
                BOND_BONDING, BOND_NONE, false, BOND_BONDING, false, BOND_NONE, BOND_NONE, false);
        testSendIntentPendingDevice(
                BOND_BONDING,
                BOND_BONDING,
                false,
                BOND_BONDING,
                false,
                BOND_NONE,
                BOND_NONE,
                false);
        testSendIntentPendingDevice(
                BOND_BONDING, BOND_BONDED, false, BOND_BONDING, false, BOND_NONE, BOND_NONE, false);
        testSendIntentPendingDevice(
                BOND_BONDING,
                badBondState,
                false,
                BOND_BONDING,
                false,
                BOND_NONE,
                BOND_NONE,
                false);
        testSendIntentPendingDevice(
                BOND_BONDED, BOND_NONE, false, BOND_NONE, true, BOND_BONDING, BOND_NONE, false);
        testSendIntentPendingDevice(
                BOND_BONDED, BOND_BONDING, false, BOND_BONDING, false, BOND_NONE, BOND_NONE, false);
        testSendIntentPendingDevice(
                BOND_BONDED, BOND_BONDED, false, BOND_BONDED, false, BOND_NONE, BOND_NONE, true);
        testSendIntentPendingDevice(
                BOND_BONDED, badBondState, false, BOND_BONDED, false, BOND_NONE, BOND_NONE, false);

        testSendIntentPendingDevice(
                BOND_NONE, BOND_NONE, true, BOND_NONE, false, BOND_NONE, BOND_NONE, false);
        testSendIntentPendingDevice(
                BOND_NONE, BOND_BONDING, true, BOND_NONE, false, BOND_NONE, BOND_NONE, false);
        testSendIntentPendingDevice(
                BOND_NONE, BOND_BONDED, true, BOND_NONE, false, BOND_NONE, BOND_NONE, false);
        testSendIntentPendingDevice(
                BOND_NONE, badBondState, true, BOND_NONE, false, BOND_NONE, BOND_NONE, false);
        testSendIntentPendingDevice(
                BOND_BONDING, BOND_NONE, true, BOND_BONDING, false, BOND_NONE, BOND_NONE, false);
        testSendIntentPendingDevice(
                BOND_BONDING, BOND_BONDING, true, BOND_BONDING, false, BOND_NONE, BOND_NONE, false);
        testSendIntentPendingDevice(
                BOND_BONDING, BOND_BONDED, true, BOND_BONDING, false, BOND_NONE, BOND_NONE, false);
        testSendIntentPendingDevice(
                BOND_BONDING, badBondState, true, BOND_BONDING, false, BOND_NONE, BOND_NONE, false);
        testSendIntentPendingDevice(
                BOND_BONDED, BOND_NONE, true, BOND_BONDED, false, BOND_NONE, BOND_NONE, false);
        testSendIntentPendingDevice(
                BOND_BONDED, BOND_BONDING, true, BOND_BONDED, false, BOND_NONE, BOND_NONE, false);
        testSendIntentPendingDevice(
                BOND_BONDED,
                BOND_BONDED,
                true,
                BOND_BONDED,
                true,
                BOND_BONDING,
                BOND_BONDED,
                false);
        testSendIntentPendingDevice(
                BOND_BONDED, badBondState, true, BOND_BONDED, false, BOND_NONE, BOND_NONE, false);

        // Uuid available, mPendingBondedDevice is empty.
        testSendIntentNoPendingDeviceWithUuid(
                BOND_NONE, BOND_NONE, false, BOND_NONE, false, BOND_NONE, BOND_NONE, false);
        testSendIntentNoPendingDeviceWithUuid(
                BOND_NONE, BOND_BONDING, false, BOND_BONDING, true, BOND_NONE, BOND_BONDING, false);
        testSendIntentNoPendingDeviceWithUuid(
                BOND_NONE, BOND_BONDED, false, BOND_BONDED, true, BOND_NONE, BOND_BONDED, false);
        testSendIntentNoPendingDeviceWithUuid(
                BOND_NONE, badBondState, false, BOND_NONE, false, BOND_NONE, BOND_NONE, false);
        testSendIntentNoPendingDeviceWithUuid(
                BOND_BONDING, BOND_NONE, false, BOND_NONE, true, BOND_BONDING, BOND_NONE, false);
        testSendIntentNoPendingDeviceWithUuid(
                BOND_BONDING,
                BOND_BONDING,
                false,
                BOND_BONDING,
                false,
                BOND_NONE,
                BOND_NONE,
                false);
        testSendIntentNoPendingDeviceWithUuid(
                BOND_BONDING,
                BOND_BONDED,
                false,
                BOND_BONDED,
                true,
                BOND_BONDING,
                BOND_BONDED,
                false);
        testSendIntentNoPendingDeviceWithUuid(
                BOND_BONDING,
                badBondState,
                false,
                BOND_BONDING,
                false,
                BOND_NONE,
                BOND_NONE,
                false);
        testSendIntentNoPendingDeviceWithUuid(
                BOND_BONDED, BOND_NONE, false, BOND_NONE, true, BOND_BONDED, BOND_NONE, false);
        testSendIntentNoPendingDeviceWithUuid(
                BOND_BONDED,
                BOND_BONDING,
                false,
                BOND_BONDING,
                true,
                BOND_BONDED,
                BOND_BONDING,
                false);
        testSendIntentNoPendingDeviceWithUuid(
                BOND_BONDED, BOND_BONDED, false, BOND_BONDED, false, BOND_NONE, BOND_NONE, false);
        testSendIntentNoPendingDeviceWithUuid(
                BOND_BONDED, badBondState, false, BOND_BONDED, false, BOND_NONE, BOND_NONE, false);

        testSendIntentNoPendingDeviceWithUuid(
                BOND_NONE, BOND_NONE, true, BOND_NONE, false, BOND_NONE, BOND_NONE, false);
        testSendIntentNoPendingDeviceWithUuid(
                BOND_NONE, BOND_BONDING, true, BOND_NONE, false, BOND_NONE, BOND_NONE, false);
        testSendIntentNoPendingDeviceWithUuid(
                BOND_NONE, BOND_BONDED, true, BOND_NONE, false, BOND_NONE, BOND_NONE, false);
        testSendIntentNoPendingDeviceWithUuid(
                BOND_NONE, badBondState, true, BOND_NONE, false, BOND_NONE, BOND_NONE, false);
        testSendIntentNoPendingDeviceWithUuid(
                BOND_BONDING, BOND_NONE, true, BOND_BONDING, false, BOND_NONE, BOND_NONE, false);
        testSendIntentNoPendingDeviceWithUuid(
                BOND_BONDING, BOND_BONDING, true, BOND_BONDING, false, BOND_NONE, BOND_NONE, false);
        testSendIntentNoPendingDeviceWithUuid(
                BOND_BONDING, BOND_BONDED, true, BOND_BONDING, false, BOND_NONE, BOND_NONE, false);
        testSendIntentNoPendingDeviceWithUuid(
                BOND_BONDING, badBondState, true, BOND_BONDING, false, BOND_NONE, BOND_NONE, false);
        testSendIntentNoPendingDeviceWithUuid(
                BOND_BONDED, BOND_NONE, true, BOND_BONDED, false, BOND_NONE, BOND_NONE, false);
        testSendIntentNoPendingDeviceWithUuid(
                BOND_BONDED, BOND_BONDING, true, BOND_BONDED, false, BOND_NONE, BOND_NONE, false);
        testSendIntentNoPendingDeviceWithUuid(
                BOND_BONDED, BOND_BONDED, true, BOND_BONDED, false, BOND_NONE, BOND_NONE, false);
        testSendIntentNoPendingDeviceWithUuid(
                BOND_BONDED, badBondState, true, BOND_BONDED, false, BOND_NONE, BOND_NONE, false);

        // Uuid available, mPendingBondedDevice contains a remote device.
        testSendIntentPendingDeviceWithUuid(
                BOND_NONE, BOND_NONE, false, BOND_NONE, false, BOND_NONE, BOND_NONE, false);
        testSendIntentPendingDeviceWithUuid(
                BOND_NONE, BOND_BONDING, false, BOND_NONE, false, BOND_NONE, BOND_NONE, false);
        testSendIntentPendingDeviceWithUuid(
                BOND_NONE, BOND_BONDED, false, BOND_NONE, false, BOND_NONE, BOND_NONE, false);
        testSendIntentPendingDeviceWithUuid(
                BOND_NONE, badBondState, false, BOND_NONE, false, BOND_NONE, BOND_NONE, false);
        testSendIntentPendingDeviceWithUuid(
                BOND_BONDING, BOND_NONE, false, BOND_BONDING, false, BOND_NONE, BOND_NONE, false);
        testSendIntentPendingDeviceWithUuid(
                BOND_BONDING,
                BOND_BONDING,
                false,
                BOND_BONDING,
                false,
                BOND_NONE,
                BOND_NONE,
                false);
        testSendIntentPendingDeviceWithUuid(
                BOND_BONDING, BOND_BONDED, false, BOND_BONDING, false, BOND_NONE, BOND_NONE, false);
        testSendIntentPendingDeviceWithUuid(
                BOND_BONDING,
                badBondState,
                false,
                BOND_BONDING,
                false,
                BOND_NONE,
                BOND_NONE,
                false);
        testSendIntentPendingDeviceWithUuid(
                BOND_BONDED, BOND_NONE, false, BOND_NONE, true, BOND_BONDING, BOND_NONE, false);
        testSendIntentPendingDeviceWithUuid(
                BOND_BONDED, BOND_BONDING, false, BOND_BONDING, false, BOND_NONE, BOND_NONE, false);
        testSendIntentPendingDeviceWithUuid(
                BOND_BONDED,
                BOND_BONDED,
                false,
                BOND_BONDED,
                true,
                BOND_BONDING,
                BOND_BONDED,
                false);
        testSendIntentPendingDeviceWithUuid(
                BOND_BONDED, badBondState, false, BOND_BONDED, false, BOND_NONE, BOND_NONE, false);

        testSendIntentPendingDeviceWithUuid(
                BOND_NONE, BOND_NONE, true, BOND_NONE, false, BOND_NONE, BOND_NONE, false);
        testSendIntentPendingDeviceWithUuid(
                BOND_NONE, BOND_BONDING, true, BOND_NONE, false, BOND_NONE, BOND_NONE, false);
        testSendIntentPendingDeviceWithUuid(
                BOND_NONE, BOND_BONDED, true, BOND_NONE, false, BOND_NONE, BOND_NONE, false);
        testSendIntentPendingDeviceWithUuid(
                BOND_NONE, badBondState, true, BOND_NONE, false, BOND_NONE, BOND_NONE, false);
        testSendIntentPendingDeviceWithUuid(
                BOND_BONDING, BOND_NONE, true, BOND_BONDING, false, BOND_NONE, BOND_NONE, false);
        testSendIntentPendingDeviceWithUuid(
                BOND_BONDING, BOND_BONDING, true, BOND_BONDING, false, BOND_NONE, BOND_NONE, false);
        testSendIntentPendingDeviceWithUuid(
                BOND_BONDING, BOND_BONDED, true, BOND_BONDING, false, BOND_NONE, BOND_NONE, false);
        testSendIntentPendingDeviceWithUuid(
                BOND_BONDING, badBondState, true, BOND_BONDING, false, BOND_NONE, BOND_NONE, false);
        testSendIntentPendingDeviceWithUuid(
                BOND_BONDED, BOND_NONE, true, BOND_BONDED, false, BOND_NONE, BOND_NONE, false);
        testSendIntentPendingDeviceWithUuid(
                BOND_BONDED, BOND_BONDING, true, BOND_BONDED, false, BOND_NONE, BOND_NONE, false);
        testSendIntentPendingDeviceWithUuid(
                BOND_BONDED,
                BOND_BONDED,
                true,
                BOND_BONDED,
                true,
                BOND_BONDING,
                BOND_BONDED,
                false);
        testSendIntentPendingDeviceWithUuid(
                BOND_BONDED, badBondState, true, BOND_BONDED, false, BOND_NONE, BOND_NONE, false);
    }

    private void testSendIntentCase(
            int oldState,
            int newState,
            boolean isTriggerFromDelayMessage,
            int expectedNewState,
            boolean shouldBroadcast,
            int broadcastOldState,
            int broadcastNewState,
            boolean shouldDelayMessageExist) {
        ArgumentCaptor<Intent> intentArgument = ArgumentCaptor.forClass(Intent.class);

        // Setup old state before start test.
        mDeviceProperties.mBondState = oldState;

        try {
            mBondStateMachine.sendIntent(
                    mDevice, newState, TEST_BOND_REASON, isTriggerFromDelayMessage);
        } catch (IllegalArgumentException e) {
            // Do nothing.
        }

        // Properties are removed when bond is removed
        if (newState != BluetoothDevice.BOND_NONE) {
            Assert.assertEquals(expectedNewState, mDeviceProperties.getBondState());
        }

        // Check for bond state Intent status.
        if (shouldBroadcast) {
            verify(mAdapterService, times(++mVerifyCount))
                    .sendBroadcastAsUser(
                            intentArgument.capture(), eq(UserHandle.ALL),
                            eq(BLUETOOTH_CONNECT), any(Bundle.class));
            verifyBondStateChangeIntent(
                    broadcastOldState, broadcastNewState, intentArgument.getValue());
        } else {
            verify(mAdapterService, times(mVerifyCount))
                    .sendBroadcastAsUser(
                            any(Intent.class),
                            any(UserHandle.class),
                            anyString(),
                            any(Bundle.class));
        }

        if (shouldDelayMessageExist) {
            Assert.assertTrue(mBondStateMachine.hasMessage(mBondStateMachine.BONDED_INTENT_DELAY));
            mBondStateMachine.removeMessage(mBondStateMachine.BONDED_INTENT_DELAY);
        } else {
            Assert.assertFalse(mBondStateMachine.hasMessage(mBondStateMachine.BONDED_INTENT_DELAY));
        }
    }

    private void testSendIntentForAllDeviceTypes(
            int oldState,
            int newState,
            boolean isTriggerFromDelayMessage,
            int expectedNewState,
            boolean shouldBroadcast,
            int broadcastOldState,
            int broadcastNewState,
            boolean shouldDelayMessageExist,
            BluetoothDevice pendingBondedDevice,
            ParcelUuid[] uuids) {
        for (int deviceType : DEVICE_TYPES) {
            resetRemoteDevice(deviceType);
            if (pendingBondedDevice != null) {
                mBondStateMachine.mPendingBondedDevices.add(mDevice);
            }
            if (uuids != null) {
                // Add dummy UUID for the device.
                mDeviceProperties.mUuids = TEST_UUIDS;
            }
            testSendIntentCase(
                    oldState,
                    newState,
                    isTriggerFromDelayMessage,
                    expectedNewState,
                    shouldBroadcast,
                    broadcastOldState,
                    broadcastNewState,
                    shouldDelayMessageExist);
        }
    }

    private void testSendIntentNoPendingDeviceWithUuid(
            int oldState,
            int newState,
            boolean isTriggerFromDelayMessage,
            int expectedNewState,
            boolean shouldBroadcast,
            int broadcastOldState,
            int broadcastNewState,
            boolean shouldDelayMessageExist) {
        testSendIntentForAllDeviceTypes(
                oldState,
                newState,
                isTriggerFromDelayMessage,
                expectedNewState,
                shouldBroadcast,
                broadcastOldState,
                broadcastNewState,
                shouldDelayMessageExist,
                null,
                TEST_UUIDS);
    }

    private void testSendIntentPendingDeviceWithUuid(
            int oldState,
            int newState,
            boolean isTriggerFromDelayMessage,
            int expectedNewState,
            boolean shouldBroadcast,
            int broadcastOldState,
            int broadcastNewState,
            boolean shouldDelayMessageExist) {
        testSendIntentForAllDeviceTypes(
                oldState,
                newState,
                isTriggerFromDelayMessage,
                expectedNewState,
                shouldBroadcast,
                broadcastOldState,
                broadcastNewState,
                shouldDelayMessageExist,
                mDevice,
                TEST_UUIDS);
    }

    private void testSendIntentPendingDevice(
            int oldState,
            int newState,
            boolean isTriggerFromDelayMessage,
            int expectedNewState,
            boolean shouldBroadcast,
            int broadcastOldState,
            int broadcastNewState,
            boolean shouldDelayMessageExist) {
        testSendIntentForAllDeviceTypes(
                oldState,
                newState,
                isTriggerFromDelayMessage,
                expectedNewState,
                shouldBroadcast,
                broadcastOldState,
                broadcastNewState,
                shouldDelayMessageExist,
                mDevice,
                null);
    }

    private void testSendIntentNoPendingDevice(
            int oldState,
            int newState,
            boolean isTriggerFromDelayMessage,
            int expectedNewState,
            boolean shouldBroadcast,
            int broadcastOldState,
            int broadcastNewState,
            boolean shouldDelayMessageExist) {
        testSendIntentForAllDeviceTypes(
                oldState,
                newState,
                isTriggerFromDelayMessage,
                expectedNewState,
                shouldBroadcast,
                broadcastOldState,
                broadcastNewState,
                shouldDelayMessageExist,
                null,
                null);
    }

    private void verifyBondStateChangeIntent(int oldState, int newState, Intent intent) {
        Assert.assertNotNull(intent);
        Assert.assertEquals(BluetoothDevice.ACTION_BOND_STATE_CHANGED, intent.getAction());
        Assert.assertEquals(mDevice, intent.getParcelableExtra(BluetoothDevice.EXTRA_DEVICE));
        Assert.assertEquals(newState, intent.getIntExtra(BluetoothDevice.EXTRA_BOND_STATE, -1));
        Assert.assertEquals(
                oldState, intent.getIntExtra(BluetoothDevice.EXTRA_PREVIOUS_BOND_STATE, -1));
        if (newState == BOND_NONE) {
            Assert.assertEquals(
                    TEST_BOND_REASON, intent.getIntExtra(BluetoothDevice.EXTRA_UNBOND_REASON, -1));
        } else {
            Assert.assertEquals(-1, intent.getIntExtra(BluetoothDevice.EXTRA_UNBOND_REASON, -1));
        }
    }
}
