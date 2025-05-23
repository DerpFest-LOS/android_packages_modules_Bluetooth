/*
 * Copyright (C) 2012 The Android Open Source Project
 * Copyright (C) 2016-2017 The Linux Foundation
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
import static android.Manifest.permission.BLUETOOTH_SCAN;

import android.annotation.NonNull;
import android.app.BroadcastOptions;
import android.bluetooth.BluetoothA2dp;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothClass;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothManager;
import android.bluetooth.BluetoothMap;
import android.bluetooth.BluetoothProfile;
import android.bluetooth.BluetoothSap;
import android.bluetooth.BluetoothUtils;
import android.bluetooth.BufferConstraint;
import android.bluetooth.BufferConstraints;
import android.content.Context;
import android.content.Intent;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelUuid;
import android.os.SystemProperties;
import android.os.UserHandle;
import android.util.Log;
import android.util.Pair;

import androidx.annotation.VisibleForTesting;

import com.android.bluetooth.BluetoothStatsLog;
import com.android.bluetooth.Utils;
import com.android.bluetooth.btservice.RemoteDevices.DeviceProperties;
import com.android.bluetooth.flags.Flags;
import com.android.modules.utils.build.SdkLevel;

import java.io.FileDescriptor;
import java.io.PrintWriter;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CopyOnWriteArrayList;

class AdapterProperties {
    private static final String TAG = AdapterProperties.class.getSimpleName();

    private static final String MAX_CONNECTED_AUDIO_DEVICES_PROPERTY =
            "persist.bluetooth.maxconnectedaudiodevices";
    private static final int MAX_CONNECTED_AUDIO_DEVICES_LOWER_BOUND = 1;
    private static final int MAX_CONNECTED_AUDIO_DEVICES_UPPER_BOUND = 5;
    private static final String A2DP_OFFLOAD_SUPPORTED_PROPERTY =
            "ro.bluetooth.a2dp_offload.supported";
    private static final String A2DP_OFFLOAD_DISABLED_PROPERTY =
            "persist.bluetooth.a2dp_offload.disabled";

    private static final long DEFAULT_DISCOVERY_TIMEOUT_MS = 12800;
    @VisibleForTesting static final int BLUETOOTH_NAME_MAX_LENGTH_BYTES = 248;
    private static final int BD_ADDR_LEN = 6; // in bytes
    private static final int SYSTEM_CONNECTION_LATENCY_METRIC = 65536;

    private volatile String mName;
    private volatile byte[] mAddress;
    private volatile BluetoothClass mBluetoothClass;
    private volatile int mScanMode;
    private volatile int mDiscoverableTimeout;
    private volatile ParcelUuid[] mUuids;

    private CopyOnWriteArrayList<BluetoothDevice> mBondedDevices = new CopyOnWriteArrayList<>();

    private int mProfilesConnecting, mProfilesConnected, mProfilesDisconnecting;
    private final HashMap<Integer, Pair<Integer, Integer>> mProfileConnectionState =
            new HashMap<>();

    private final CompletableFuture<List<BufferConstraint>> mBufferConstraintList =
            new CompletableFuture<>();

    private volatile int mConnectionState = BluetoothAdapter.STATE_DISCONNECTED;
    private volatile int mState = BluetoothAdapter.STATE_OFF;
    private int mMaxConnectedAudioDevices = 1;
    private boolean mA2dpOffloadEnabled = false;

    private final AdapterService mService;
    private final BluetoothAdapter mAdapter;
    private final RemoteDevices mRemoteDevices;
    private final Handler mHandler;

    private boolean mDiscovering;
    private long mDiscoveryEndMs; // < Time (ms since epoch) that discovery ended or will end.
    // TODO - all hw capabilities to be exposed as a class
    private int mNumOfAdvertisementInstancesSupported;
    private boolean mRpaOffloadSupported;
    private int mNumOfOffloadedIrkSupported;
    private int mNumOfOffloadedScanFilterSupported;
    private int mOffloadedScanResultStorageBytes;
    private int mVersSupported;
    private int mTotNumOfTrackableAdv;
    private boolean mIsExtendedScanSupported;
    private boolean mIsDebugLogSupported;
    private boolean mIsActivityAndEnergyReporting;
    private boolean mIsLe2MPhySupported;
    private boolean mIsLeCodedPhySupported;
    private boolean mIsLeExtendedAdvertisingSupported;
    private boolean mIsLePeriodicAdvertisingSupported;
    private int mLeMaximumAdvertisingDataLength;
    private boolean mIsOffloadedTransportDiscoveryDataScanSupported;

    private int mIsDynamicAudioBufferSizeSupported;
    private int mDynamicAudioBufferSizeSupportedCodecsGroup1;
    private int mDynamicAudioBufferSizeSupportedCodecsGroup2;

    private boolean mIsLePeriodicAdvertisingSyncTransferSenderSupported;
    private boolean mIsLePeriodicAdvertisingSyncTransferRecipientSupported;
    private boolean mIsLeConnectedIsochronousStreamCentralSupported;
    private boolean mIsLeIsochronousBroadcasterSupported;
    private boolean mIsLeChannelSoundingSupported;

    private int mNumberOfSupportedOffloadedLeCocSockets;
    private int mNumberOfSupportedOffloadedRfcommSockets = 0;

    // Lock for all getters and setters.
    // If finer grained locking is needer, more locks
    // can be added here.
    private final Object mObject = new Object();

    AdapterProperties(AdapterService service, RemoteDevices remoteDevices, Looper looper) {
        mAdapter = ((Context) service).getSystemService(BluetoothManager.class).getAdapter();
        mRemoteDevices = remoteDevices;
        mService = service;
        mHandler = new Handler(looper);
        invalidateBluetoothCaches();
    }

    public void init() {
        mProfileConnectionState.clear();

        // Get default max connected audio devices from config.xml
        int configDefaultMaxConnectedAudioDevices =
                mService.getResources()
                        .getInteger(
                                com.android.bluetooth.R.integer
                                        .config_bluetooth_max_connected_audio_devices);
        // Override max connected audio devices if MAX_CONNECTED_AUDIO_DEVICES_PROPERTY is set
        int propertyOverlayedMaxConnectedAudioDevices =
                SystemProperties.getInt(
                        MAX_CONNECTED_AUDIO_DEVICES_PROPERTY,
                        configDefaultMaxConnectedAudioDevices);
        // Make sure the final value of max connected audio devices is within allowed range
        mMaxConnectedAudioDevices =
                Math.min(
                        Math.max(
                                propertyOverlayedMaxConnectedAudioDevices,
                                MAX_CONNECTED_AUDIO_DEVICES_LOWER_BOUND),
                        MAX_CONNECTED_AUDIO_DEVICES_UPPER_BOUND);
        Log.i(
                TAG,
                "init(), maxConnectedAudioDevices, default="
                        + configDefaultMaxConnectedAudioDevices
                        + ", propertyOverlayed="
                        + propertyOverlayedMaxConnectedAudioDevices
                        + ", finalValue="
                        + mMaxConnectedAudioDevices);

        mA2dpOffloadEnabled =
                SystemProperties.getBoolean(A2DP_OFFLOAD_SUPPORTED_PROPERTY, false)
                        && !SystemProperties.getBoolean(A2DP_OFFLOAD_DISABLED_PROPERTY, false);

        invalidateBluetoothCaches();
    }

    public void cleanup() {
        mProfileConnectionState.clear();

        mBondedDevices.clear();
        invalidateBluetoothCaches();
    }

    private static void invalidateGetProfileConnectionStateCache() {
        BluetoothAdapter.invalidateGetProfileConnectionStateCache();
    }

    private static void invalidateIsOffloadedFilteringSupportedCache() {
        BluetoothAdapter.invalidateIsOffloadedFilteringSupportedCache();
    }

    private static void invalidateBluetoothGetConnectionStateCache() {
        BluetoothMap.invalidateBluetoothGetConnectionStateCache();
        BluetoothSap.invalidateBluetoothGetConnectionStateCache();
    }

    private static void invalidateGetConnectionStateCache() {
        BluetoothAdapter.invalidateGetAdapterConnectionStateCache();
    }

    private static void invalidateGetBondStateCache() {
        BluetoothDevice.invalidateBluetoothGetBondStateCache();
    }

    private static void invalidateBluetoothCaches() {
        invalidateGetProfileConnectionStateCache();
        invalidateIsOffloadedFilteringSupportedCache();
        invalidateGetConnectionStateCache();
        invalidateGetBondStateCache();
        invalidateBluetoothGetConnectionStateCache();
    }

    @Override
    public Object clone() throws CloneNotSupportedException {
        throw new CloneNotSupportedException();
    }

    /**
     * @return the mName
     */
    String getName() {
        return mName;
    }

    /**
     * Set the local adapter property - name
     *
     * @param name the name to set
     */
    boolean setName(String name) {
        synchronized (mObject) {
            return mService.getNative()
                    .setAdapterProperty(
                            AbstractionLayer.BT_PROPERTY_BDNAME,
                            Utils.truncateStringForUtf8Storage(
                                            name, BLUETOOTH_NAME_MAX_LENGTH_BYTES)
                                    .getBytes());
        }
    }

    /**
     * @return the mUuids
     */
    ParcelUuid[] getUuids() {
        return mUuids;
    }

    /**
     * @return the mAddress
     */
    byte[] getAddress() {
        return mAddress;
    }

    /**
     * @param connectionState the mConnectionState to set
     */
    void setConnectionState(int connectionState) {
        mConnectionState = connectionState;
        invalidateGetConnectionStateCache();
    }

    /**
     * @return the mConnectionState
     */
    int getConnectionState() {
        return mConnectionState;
    }

    /**
     * @param state the mState to set
     */
    void setState(int state) {
        debugLog("Setting state to " + BluetoothAdapter.nameForState(state));
        mState = state;
    }

    /**
     * @return the mState
     */
    int getState() {
        return mState;
    }

    /**
     * @return the mNumOfAdvertisementInstancesSupported
     */
    int getNumOfAdvertisementInstancesSupported() {
        return mNumOfAdvertisementInstancesSupported;
    }

    /**
     * @return the mRpaOffloadSupported
     */
    boolean isRpaOffloadSupported() {
        return mRpaOffloadSupported;
    }

    /**
     * @return the mNumOfOffloadedIrkSupported
     */
    int getNumOfOffloadedIrkSupported() {
        return mNumOfOffloadedIrkSupported;
    }

    /**
     * @return the mNumOfOffloadedScanFilterSupported
     */
    int getNumOfOffloadedScanFilterSupported() {
        return mNumOfOffloadedScanFilterSupported;
    }

    /**
     * @return the mOffloadedScanResultStorageBytes
     */
    int getOffloadedScanResultStorage() {
        return mOffloadedScanResultStorageBytes;
    }

    /**
     * @return tx/rx/idle activity and energy info
     */
    boolean isActivityAndEnergyReportingSupported() {
        return mIsActivityAndEnergyReporting;
    }

    /**
     * @return the mIsLe2MPhySupported
     */
    boolean isLe2MPhySupported() {
        return mIsLe2MPhySupported;
    }

    /**
     * @return the mIsLeCodedPhySupported
     */
    boolean isLeCodedPhySupported() {
        return mIsLeCodedPhySupported;
    }

    /**
     * @return the mIsLeExtendedAdvertisingSupported
     */
    boolean isLeExtendedAdvertisingSupported() {
        return mIsLeExtendedAdvertisingSupported;
    }

    /**
     * @return the mIsLePeriodicAdvertisingSupported
     */
    boolean isLePeriodicAdvertisingSupported() {
        return mIsLePeriodicAdvertisingSupported;
    }

    /**
     * @return the mIsLePeriodicAdvertisingSyncTransferSenderSupported
     */
    boolean isLePeriodicAdvertisingSyncTransferSenderSupported() {
        return mIsLePeriodicAdvertisingSyncTransferSenderSupported;
    }

    /**
     * @return the mIsLePeriodicAdvertisingSyncTransferRecipientSupported
     */
    boolean isLePeriodicAdvertisingSyncTransferRecipientSupported() {
        return mIsLePeriodicAdvertisingSyncTransferRecipientSupported;
    }

    /**
     * @return the mIsLeConnectedIsochronousStreamCentralSupported
     */
    boolean isLeConnectedIsochronousStreamCentralSupported() {
        return mIsLeConnectedIsochronousStreamCentralSupported;
    }

    /**
     * @return the mIsLeIsochronousBroadcasterSupported
     */
    boolean isLeIsochronousBroadcasterSupported() {
        return mIsLeIsochronousBroadcasterSupported;
    }

    /**
     * @return the mIsLeChannelSoundingSupported
     */
    boolean isLeChannelSoundingSupported() {
        return mIsLeChannelSoundingSupported;
    }

    /**
     * @return the getLeMaximumAdvertisingDataLength
     */
    int getLeMaximumAdvertisingDataLength() {
        return mLeMaximumAdvertisingDataLength;
    }

    /**
     * @return total number of trackable advertisements
     */
    int getTotalNumOfTrackableAdvertisements() {
        return mTotNumOfTrackableAdv;
    }

    /**
     * @return the isOffloadedTransportDiscoveryDataScanSupported
     */
    public boolean isOffloadedTransportDiscoveryDataScanSupported() {
        return mIsOffloadedTransportDiscoveryDataScanSupported;
    }

    /**
     * @return the maximum number of connected audio devices
     */
    int getMaxConnectedAudioDevices() {
        return mMaxConnectedAudioDevices;
    }

    /**
     * @return A2DP offload support
     */
    boolean isA2dpOffloadEnabled() {
        return mA2dpOffloadEnabled;
    }

    /**
     * @return Dynamic Audio Buffer support
     */
    int getDynamicBufferSupport() {
        if (!mA2dpOffloadEnabled) {
            // TODO: Enable Dynamic Audio Buffer for A2DP software encoding when ready.
            mIsDynamicAudioBufferSizeSupported = BluetoothA2dp.DYNAMIC_BUFFER_SUPPORT_NONE;
        } else {
            if ((mDynamicAudioBufferSizeSupportedCodecsGroup1 != 0)
                    || (mDynamicAudioBufferSizeSupportedCodecsGroup2 != 0)) {
                mIsDynamicAudioBufferSizeSupported =
                        BluetoothA2dp.DYNAMIC_BUFFER_SUPPORT_A2DP_OFFLOAD;
            } else {
                mIsDynamicAudioBufferSizeSupported = BluetoothA2dp.DYNAMIC_BUFFER_SUPPORT_NONE;
            }
        }
        return mIsDynamicAudioBufferSizeSupported;
    }

    /**
     * @return Dynamic Audio Buffer Capability
     */
    BufferConstraints getBufferConstraints() {
        return new BufferConstraints(mBufferConstraintList.join());
    }

    /**
     * Set the dynamic audio buffer size
     *
     * @param codec the codecs to set
     * @param size the size to set
     */
    boolean setBufferLengthMillis(int codec, int size) {
        return mService.getNative().setBufferLengthMillis(codec, size);
    }

    /**
     * @return the mBondedDevices
     */
    BluetoothDevice[] getBondedDevices() {
        BluetoothDevice[] bondedDeviceList = new BluetoothDevice[0];
        try {
            bondedDeviceList = mBondedDevices.toArray(bondedDeviceList);
        } catch (ArrayStoreException ee) {
            Log.e(TAG, "Error retrieving bonded device array");
        }
        infoLog("getBondedDevices: length=" + bondedDeviceList.length);
        return bondedDeviceList;
    }

    // This function shall be invoked from BondStateMachine whenever the bond
    // state changes.
    @VisibleForTesting
    void onBondStateChanged(BluetoothDevice device, int state) {
        if (device == null) {
            Log.w(TAG, "onBondStateChanged, device is null");
            return;
        }
        try {
            byte[] addrByte = Utils.getByteAddress(device);
            DeviceProperties prop = mRemoteDevices.getDeviceProperties(device);
            if (prop == null) {
                prop = mRemoteDevices.addDeviceProperties(addrByte);
            }
            device = prop.getDevice();
            prop.setBondState(state);

            if (state == BluetoothDevice.BOND_BONDED) {
                // add if not already in list
                if (!mBondedDevices.contains(device)) {
                    debugLog("Adding bonded device:" + device);
                    mBondedDevices.add(device);
                    cleanupPrevBondRecordsFor(device);
                }
            } else if (state == BluetoothDevice.BOND_NONE) {
                // remove device from list
                if (mBondedDevices.remove(device)) {
                    debugLog("Removing bonded device:" + device);
                } else {
                    debugLog("Failed to remove device: " + device);
                }
            }
            invalidateGetBondStateCache();
        } catch (Exception ee) {
            Log.w(TAG, "onBondStateChanged: Exception ", ee);
        }
    }

    void cleanupPrevBondRecordsFor(BluetoothDevice device) {
        String address = device.getAddress();
        String identityAddress =
                Flags.identityAddressNullIfNotKnown()
                        ? Utils.getBrEdrAddress(device, mService)
                        : mService.getIdentityAddress(address);
        int deviceType = mRemoteDevices.getDeviceProperties(device).getDeviceType();
        debugLog("cleanupPrevBondRecordsFor: " + device + ", device type: " + deviceType);
        if (identityAddress == null) {
            return;
        }

        if (deviceType != BluetoothDevice.DEVICE_TYPE_LE) {
            return;
        }

        for (BluetoothDevice existingDevice : mBondedDevices) {
            String existingAddress = existingDevice.getAddress();
            String existingIdentityAddress =
                    Flags.identityAddressNullIfNotKnown()
                            ? Utils.getBrEdrAddress(existingDevice, mService)
                            : mService.getIdentityAddress(existingAddress);
            int existingDeviceType =
                    mRemoteDevices.getDeviceProperties(existingDevice).getDeviceType();

            boolean removeExisting = false;
            if (identityAddress.equals(existingIdentityAddress)
                    && !address.equals(existingAddress)) {
                // Existing device record should be removed only if the device type is LE-only
                removeExisting = (existingDeviceType == BluetoothDevice.DEVICE_TYPE_LE);
            }

            if (removeExisting) {
                // Found an existing LE-only device with the same identity address but different
                // pseudo address
                if (mService.getNative().removeBond(Utils.getBytesFromAddress(existingAddress))) {
                    mBondedDevices.remove(existingDevice);
                    infoLog(
                            "Removing old bond record: "
                                    + existingDevice
                                    + " for the device: "
                                    + device);
                } else {
                    Log.e(
                            TAG,
                            "Unexpected error while removing old bond record:"
                                    + existingDevice
                                    + " for the device: "
                                    + device);
                }
                break;
            }
        }
    }

    int getDiscoverableTimeout() {
        return mDiscoverableTimeout;
    }

    boolean setDiscoverableTimeout(int timeout) {
        synchronized (mObject) {
            return mService.getNative()
                    .setAdapterProperty(
                            AbstractionLayer.BT_PROPERTY_ADAPTER_DISCOVERABLE_TIMEOUT,
                            Utils.intToByteArray(timeout));
        }
    }

    int getProfileConnectionState(int profile) {
        synchronized (mObject) {
            Pair<Integer, Integer> p = mProfileConnectionState.get(profile);
            if (p != null) {
                return p.first;
            }
            return BluetoothProfile.STATE_DISCONNECTED;
        }
    }

    long discoveryEndMillis() {
        return mDiscoveryEndMs;
    }

    boolean isDiscovering() {
        return mDiscovering;
    }


    void updateOnProfileConnectionChanged(
            BluetoothDevice device, int profile, int newState, int prevState) {
        String logInfo =
                ("profile=" + BluetoothProfile.getProfileName(profile))
                        + (" device=" + device)
                        + (" state [" + prevState + " -> " + newState + "]");
        Log.d(TAG, "updateOnProfileConnectionChanged: " + logInfo);
        if (!isNormalStateTransition(prevState, newState)) {
            Log.w(TAG, "updateOnProfileConnectionChanged: Unexpected transition. " + logInfo);
        }
        BluetoothStatsLog.write(
                BluetoothStatsLog.BLUETOOTH_CONNECTION_STATE_CHANGED,
                newState,
                0 /* deprecated */,
                profile,
                mService.obfuscateAddress(device),
                mService.getMetricId(device),
                0,
                SYSTEM_CONNECTION_LATENCY_METRIC);
        if (!validateProfileConnectionState(newState)
                || !validateProfileConnectionState(prevState)) {
            // Previously, an invalid state was broadcast anyway,
            // with the invalid state converted to -1 in the intent.
            // Better to log an error and not send an intent with
            // invalid contents or set mAdapterConnectionState to -1.
            Log.e(TAG, "updateOnProfileConnectionChanged: Invalid transition. " + logInfo);
            return;
        }

        synchronized (mObject) {
            updateProfileConnectionState(profile, newState, prevState);

            if (updateCountersAndCheckForConnectionStateChange(newState, prevState)) {
                int newAdapterState = convertToAdapterState(newState);
                int prevAdapterState = convertToAdapterState(prevState);
                setConnectionState(newAdapterState);

                Intent intent =
                        new Intent(BluetoothAdapter.ACTION_CONNECTION_STATE_CHANGED)
                                .putExtra(BluetoothDevice.EXTRA_DEVICE, device)
                                .putExtra(BluetoothAdapter.EXTRA_CONNECTION_STATE, newAdapterState)
                                .putExtra(
                                        BluetoothAdapter.EXTRA_PREVIOUS_CONNECTION_STATE,
                                        prevAdapterState)
                                .addFlags(Intent.FLAG_RECEIVER_REGISTERED_ONLY_BEFORE_BOOT);
                MetricsLogger.getInstance()
                        .logProfileConnectionStateChange(device, profile, newState, prevState);
                Log.d(TAG, "updateOnProfileConnectionChanged: " + logInfo);
                mService.sendBroadcastAsUser(
                        intent,
                        UserHandle.ALL,
                        BLUETOOTH_CONNECT,
                        Utils.getTempBroadcastOptions().toBundle());
            }
        }
    }



    private boolean validateProfileConnectionState(int state) {
        return (state == BluetoothProfile.STATE_DISCONNECTED
                || state == BluetoothProfile.STATE_CONNECTING
                || state == BluetoothProfile.STATE_CONNECTED
                || state == BluetoothProfile.STATE_DISCONNECTING);
    }

    private static int convertToAdapterState(int state) {
        switch (state) {
            case BluetoothProfile.STATE_DISCONNECTED:
                return BluetoothAdapter.STATE_DISCONNECTED;
            case BluetoothProfile.STATE_DISCONNECTING:
                return BluetoothAdapter.STATE_DISCONNECTING;
            case BluetoothProfile.STATE_CONNECTED:
                return BluetoothAdapter.STATE_CONNECTED;
            case BluetoothProfile.STATE_CONNECTING:
                return BluetoothAdapter.STATE_CONNECTING;
        }
        Log.e(TAG, "convertToAdapterState, unknow state " + state);
        return -1;
    }

    private static boolean isNormalStateTransition(int prevState, int nextState) {
        switch (prevState) {
            case BluetoothProfile.STATE_DISCONNECTED:
                return nextState == BluetoothProfile.STATE_CONNECTING;
            case BluetoothProfile.STATE_CONNECTED:
                return nextState == BluetoothProfile.STATE_DISCONNECTING;
            case BluetoothProfile.STATE_DISCONNECTING:
            case BluetoothProfile.STATE_CONNECTING:
                return (nextState == BluetoothProfile.STATE_DISCONNECTED)
                        || (nextState == BluetoothProfile.STATE_CONNECTED);
            default:
                return false;
        }
    }

    private boolean updateCountersAndCheckForConnectionStateChange(int state, int prevState) {
        switch (prevState) {
            case BluetoothProfile.STATE_CONNECTING:
                if (mProfilesConnecting > 0) {
                    mProfilesConnecting--;
                } else {
                    Log.e(TAG, "mProfilesConnecting " + mProfilesConnecting);
                    throw new IllegalStateException(
                            "Invalid state transition, " + prevState + " -> " + state);
                }
                break;

            case BluetoothProfile.STATE_CONNECTED:
                if (mProfilesConnected > 0) {
                    mProfilesConnected--;
                } else {
                    Log.e(TAG, "mProfilesConnected " + mProfilesConnected);
                    throw new IllegalStateException(
                            "Invalid state transition, " + prevState + " -> " + state);
                }
                break;

            case BluetoothProfile.STATE_DISCONNECTING:
                if (mProfilesDisconnecting > 0) {
                    mProfilesDisconnecting--;
                } else {
                    Log.e(TAG, "mProfilesDisconnecting " + mProfilesDisconnecting);
                    throw new IllegalStateException(
                            "Invalid state transition, " + prevState + " -> " + state);
                }
                break;
        }

        switch (state) {
            case BluetoothProfile.STATE_CONNECTING:
                mProfilesConnecting++;
                return (mProfilesConnected == 0 && mProfilesConnecting == 1);

            case BluetoothProfile.STATE_CONNECTED:
                mProfilesConnected++;
                return (mProfilesConnected == 1);

            case BluetoothProfile.STATE_DISCONNECTING:
                mProfilesDisconnecting++;
                return (mProfilesConnected == 0 && mProfilesDisconnecting == 1);

            case BluetoothProfile.STATE_DISCONNECTED:
                return (mProfilesConnected == 0 && mProfilesConnecting == 0);

            default:
                return true;
        }
    }

    private void updateProfileConnectionState(int profile, int newState, int oldState) {
        // mProfileConnectionState is a hashmap -
        // <Integer, Pair<Integer, Integer>>
        // The key is the profile, the value is a pair. first element
        // is the state and the second element is the number of devices
        // in that state.
        int numDev = 1;
        int newHashState = newState;
        boolean update = true;

        // The following conditions are considered in this function:
        // 1. If there is no record of profile and state - update
        // 2. If a new device's state is current hash state - increment
        //    number of devices in the state.
        // 3. If a state change has happened to Connected or Connecting
        //    (if current state is not connected), update.
        // 4. If numDevices is 1 and that device state is being updated, update
        // 5. If numDevices is > 1 and one of the devices is changing state,
        //    decrement numDevices but maintain oldState if it is Connected or
        //    Connecting
        Pair<Integer, Integer> stateNumDev = mProfileConnectionState.get(profile);
        if (stateNumDev != null) {
            int currHashState = stateNumDev.first;
            numDev = stateNumDev.second;

            if (newState == currHashState) {
                numDev++;
            } else if (newState == BluetoothProfile.STATE_CONNECTED
                    || (newState == BluetoothProfile.STATE_CONNECTING
                            && currHashState != BluetoothProfile.STATE_CONNECTED)) {
                numDev = 1;
            } else if (numDev == 1 && oldState == currHashState) {
                update = true;
            } else if (numDev > 1 && oldState == currHashState) {
                numDev--;

                if (currHashState == BluetoothProfile.STATE_CONNECTED
                        || currHashState == BluetoothProfile.STATE_CONNECTING) {
                    newHashState = currHashState;
                }
            } else {
                update = false;
            }
        }

        if (update) {
            mProfileConnectionState.put(profile, new Pair<Integer, Integer>(newHashState, numDev));
            invalidateGetProfileConnectionStateCache();
        }
    }

    void adapterPropertyChangedCallback(int[] types, byte[][] values) {
        if (Flags.adapterPropertiesLooper()) {
            mHandler.post(() -> adapterPropertyChangedCallbackInternal(types, values));
        } else {
            adapterPropertyChangedCallbackInternal(types, values);
        }
    }

    private void adapterPropertyChangedCallbackInternal(int[] types, byte[][] values) {
        Intent intent;
        int type;
        byte[] val;
        for (int i = 0; i < types.length; i++) {
            val = values[i];
            type = types[i];
            infoLog("adapterPropertyChangedCallback with type:" + type + " len:" + val.length);
            synchronized (mObject) {
                switch (type) {
                    case AbstractionLayer.BT_PROPERTY_BDNAME:
                        String name = new String(val);
                        if (Flags.getNameAndAddressAsCallback() && name.equals(mName)) {
                            debugLog("Name already set: " + mName);
                            break;
                        }
                        mName = name;
                        if (Flags.getNameAndAddressAsCallback()) {
                            mService.updateAdapterName(mName);
                            break;
                        }
                        intent = new Intent(BluetoothAdapter.ACTION_LOCAL_NAME_CHANGED);
                        intent.putExtra(BluetoothAdapter.EXTRA_LOCAL_NAME, mName);
                        intent.addFlags(Intent.FLAG_RECEIVER_REGISTERED_ONLY_BEFORE_BOOT);
                        mService.sendBroadcastAsUser(
                                intent,
                                UserHandle.ALL,
                                BLUETOOTH_CONNECT,
                                Utils.getTempBroadcastOptions().toBundle());
                        debugLog("Name is: " + mName);
                        break;
                    case AbstractionLayer.BT_PROPERTY_BDADDR:
                        if (Flags.getNameAndAddressAsCallback() && Arrays.equals(mAddress, val)) {
                            debugLog("Address already set");
                            break;
                        }
                        mAddress = val;
                        String address = Utils.getAddressStringFromByte(mAddress);
                        if (Flags.getNameAndAddressAsCallback()) {
                            mService.updateAdapterAddress(address);
                            // ACTION_BLUETOOTH_ADDRESS_CHANGED is redundant
                            break;
                        }
                        intent = new Intent(BluetoothAdapter.ACTION_BLUETOOTH_ADDRESS_CHANGED);
                        intent.putExtra(BluetoothAdapter.EXTRA_BLUETOOTH_ADDRESS, address);
                        intent.addFlags(Intent.FLAG_RECEIVER_REGISTERED_ONLY_BEFORE_BOOT);
                        mService.sendBroadcastAsUser(
                                intent,
                                UserHandle.ALL,
                                BLUETOOTH_CONNECT,
                                Utils.getTempBroadcastOptions().toBundle());
                        break;
                    case AbstractionLayer.BT_PROPERTY_CLASS_OF_DEVICE:
                        if (val == null || val.length != 3) {
                            debugLog("Invalid BT CoD value from stack.");
                            return;
                        }
                        int bluetoothClass =
                                ((int) val[0] << 16) + ((int) val[1] << 8) + (int) val[2];
                        if (bluetoothClass != 0) {
                            mBluetoothClass = new BluetoothClass(bluetoothClass);
                        }
                        debugLog("BT Class:" + mBluetoothClass);
                        break;
                    case AbstractionLayer.BT_PROPERTY_UUIDS:
                        mUuids = Utils.byteArrayToUuid(val);
                        break;
                    case AbstractionLayer.BT_PROPERTY_ADAPTER_BONDED_DEVICES:
                        int number = val.length / BD_ADDR_LEN;
                        byte[] addrByte = new byte[BD_ADDR_LEN];
                        for (int j = 0; j < number; j++) {
                            System.arraycopy(val, j * BD_ADDR_LEN, addrByte, 0, BD_ADDR_LEN);
                            onBondStateChanged(
                                    mAdapter.getRemoteDevice(
                                            Utils.getAddressStringFromByte(addrByte)),
                                    BluetoothDevice.BOND_BONDED);
                        }
                        break;
                    case AbstractionLayer.BT_PROPERTY_ADAPTER_DISCOVERABLE_TIMEOUT:
                        mDiscoverableTimeout = Utils.byteArrayToInt(val, 0);
                        debugLog("Discoverable Timeout:" + mDiscoverableTimeout);
                        break;

                    case AbstractionLayer.BT_PROPERTY_LOCAL_LE_FEATURES:
                        updateFeatureSupport(val);
                        mService.updateLeAudioProfileServiceState();
                        break;

                    case AbstractionLayer.BT_PROPERTY_DYNAMIC_AUDIO_BUFFER:
                        updateDynamicAudioBufferSupport(val);
                        break;

                    case AbstractionLayer.BT_PROPERTY_LPP_OFFLOAD_FEATURES:
                        updateLppOffloadFeatureSupport(val);
                        break;

                    default:
                        Log.e(TAG, "Property change not handled in Java land:" + type);
                }
            }
        }
    }

    private void updateFeatureSupport(byte[] val) {
        mVersSupported = ((0xFF & ((int) val[1])) << 8) + (0xFF & ((int) val[0]));
        mNumOfAdvertisementInstancesSupported = (0xFF & ((int) val[3]));
        mRpaOffloadSupported = ((0xFF & ((int) val[4])) != 0);
        mNumOfOffloadedIrkSupported = (0xFF & ((int) val[5]));
        mNumOfOffloadedScanFilterSupported = (0xFF & ((int) val[6]));
        mIsActivityAndEnergyReporting = ((0xFF & ((int) val[7])) != 0);
        mOffloadedScanResultStorageBytes = ((0xFF & ((int) val[9])) << 8) + (0xFF & ((int) val[8]));
        mTotNumOfTrackableAdv = ((0xFF & ((int) val[11])) << 8) + (0xFF & ((int) val[10]));
        mIsExtendedScanSupported = ((0xFF & ((int) val[12])) != 0);
        mIsDebugLogSupported = ((0xFF & ((int) val[13])) != 0);
        mIsLe2MPhySupported = ((0xFF & ((int) val[14])) != 0);
        mIsLeCodedPhySupported = ((0xFF & ((int) val[15])) != 0);
        mIsLeExtendedAdvertisingSupported = ((0xFF & ((int) val[16])) != 0);
        mIsLePeriodicAdvertisingSupported = ((0xFF & ((int) val[17])) != 0);
        mLeMaximumAdvertisingDataLength =
                (0xFF & ((int) val[18])) + ((0xFF & ((int) val[19])) << 8);
        mDynamicAudioBufferSizeSupportedCodecsGroup1 =
                ((0xFF & ((int) val[21])) << 8) + (0xFF & ((int) val[20]));
        mDynamicAudioBufferSizeSupportedCodecsGroup2 =
                ((0xFF & ((int) val[23])) << 8) + (0xFF & ((int) val[22]));
        mIsLePeriodicAdvertisingSyncTransferSenderSupported = ((0xFF & ((int) val[24])) != 0);
        mIsLeConnectedIsochronousStreamCentralSupported = ((0xFF & ((int) val[25])) != 0);
        mIsLeIsochronousBroadcasterSupported = ((0xFF & ((int) val[26])) != 0);
        mIsLePeriodicAdvertisingSyncTransferRecipientSupported = ((0xFF & ((int) val[27])) != 0);
        mIsOffloadedTransportDiscoveryDataScanSupported = ((0x01 & ((int) val[28])) != 0);
        mIsLeChannelSoundingSupported = ((0xFF & ((int) val[30])) != 0);

        Log.d(
                TAG,
                "BT_PROPERTY_LOCAL_LE_FEATURES: update from BT controller"
                        + " mNumOfAdvertisementInstancesSupported = "
                        + mNumOfAdvertisementInstancesSupported
                        + " mRpaOffloadSupported = "
                        + mRpaOffloadSupported
                        + " mNumOfOffloadedIrkSupported = "
                        + mNumOfOffloadedIrkSupported
                        + " mNumOfOffloadedScanFilterSupported = "
                        + mNumOfOffloadedScanFilterSupported
                        + " mOffloadedScanResultStorageBytes= "
                        + mOffloadedScanResultStorageBytes
                        + " mIsActivityAndEnergyReporting = "
                        + mIsActivityAndEnergyReporting
                        + " mVersSupported = "
                        + mVersSupported
                        + " mTotNumOfTrackableAdv = "
                        + mTotNumOfTrackableAdv
                        + " mIsExtendedScanSupported = "
                        + mIsExtendedScanSupported
                        + " mIsDebugLogSupported = "
                        + mIsDebugLogSupported
                        + " mIsLe2MPhySupported = "
                        + mIsLe2MPhySupported
                        + " mIsLeCodedPhySupported = "
                        + mIsLeCodedPhySupported
                        + " mIsLeExtendedAdvertisingSupported = "
                        + mIsLeExtendedAdvertisingSupported
                        + " mIsLePeriodicAdvertisingSupported = "
                        + mIsLePeriodicAdvertisingSupported
                        + " mLeMaximumAdvertisingDataLength = "
                        + mLeMaximumAdvertisingDataLength
                        + " mDynamicAudioBufferSizeSupportedCodecsGroup1 = "
                        + mDynamicAudioBufferSizeSupportedCodecsGroup1
                        + " mDynamicAudioBufferSizeSupportedCodecsGroup2 = "
                        + mDynamicAudioBufferSizeSupportedCodecsGroup2
                        + " mIsLePeriodicAdvertisingSyncTransferSenderSupported = "
                        + mIsLePeriodicAdvertisingSyncTransferSenderSupported
                        + " mIsLeConnectedIsochronousStreamCentralSupported = "
                        + mIsLeConnectedIsochronousStreamCentralSupported
                        + " mIsLeIsochronousBroadcasterSupported = "
                        + mIsLeIsochronousBroadcasterSupported
                        + " mIsLePeriodicAdvertisingSyncTransferRecipientSupported = "
                        + mIsLePeriodicAdvertisingSyncTransferRecipientSupported
                        + " mIsOffloadedTransportDiscoveryDataScanSupported = "
                        + mIsOffloadedTransportDiscoveryDataScanSupported
                        + " mIsLeChannelSoundingSupported = "
                        + mIsLeChannelSoundingSupported);
        invalidateIsOffloadedFilteringSupportedCache();
    }

    private void updateDynamicAudioBufferSupport(byte[] val) {
        if (mBufferConstraintList.isDone()) {
            return;
        }

        // bufferConstraints is the table indicates the capability of all the codecs
        // with buffer time. The raw is codec number, and the column is buffer type. There are 3
        // buffer types - default/maximum/minimum.
        // The maximum number of raw is BUFFER_CODEC_MAX_NUM(32).
        // The maximum number of column is BUFFER_TYPE_MAX(3).
        // The array element indicates the buffer time, the size is two octet.
        List<BufferConstraint> bufferConstraintList = new ArrayList<BufferConstraint>();

        for (int i = 0; i < BufferConstraints.BUFFER_CODEC_MAX_NUM; i++) {
            int defaultBufferTime =
                    ((0xFF & ((int) val[i * 6 + 1])) << 8) + (0xFF & ((int) val[i * 6]));
            int maximumBufferTime =
                    ((0xFF & ((int) val[i * 6 + 3])) << 8) + (0xFF & ((int) val[i * 6 + 2]));
            int minimumBufferTime =
                    ((0xFF & ((int) val[i * 6 + 5])) << 8) + (0xFF & ((int) val[i * 6 + 4]));
            bufferConstraintList.add(
                    new BufferConstraint(defaultBufferTime, maximumBufferTime, minimumBufferTime));
        }

        mBufferConstraintList.complete(bufferConstraintList);
    }

    /**
     * @return the mNumberOfSupportedOffloadedLeCocSockets
     */
    int getNumberOfSupportedOffloadedLeCocSockets() {
        return mNumberOfSupportedOffloadedLeCocSockets;
    }

    /**
     * @return the mNumberOfSupportedOffloadedRfcommSockets
     */
    int getNumberOfSupportedOffloadedRfcommSockets() {
        return mNumberOfSupportedOffloadedRfcommSockets;
    }

    private void updateLppOffloadFeatureSupport(byte[] val) {
        if (val.length < 1) {
            Log.e(TAG, "BT_PROPERTY_LPP_OFFLOAD_FEATURES: invalid value length");
            return;
        }
        // TODO(b/342012881) Read mNumberOfSupportedOffloadedRfcommSockets from host stack
        mNumberOfSupportedOffloadedLeCocSockets = (0xFF & ((int) val[0]));

        Log.d(
                TAG,
                "BT_PROPERTY_LPP_OFFLOAD_FEATURES: update from Offload HAL"
                        + " mNumberOfSupportedOffloadedLeCocSockets = "
                        + mNumberOfSupportedOffloadedLeCocSockets
                        + " mNumberOfSupportedOffloadedRfcommSockets = "
                        + mNumberOfSupportedOffloadedRfcommSockets);
    }

    void onBluetoothReady() {
        debugLog(
                "onBluetoothReady, state="
                        + BluetoothAdapter.nameForState(getState())
                        + ", ScanMode="
                        + mScanMode);

        synchronized (mObject) {
            // Reset adapter and profile connection states
            setConnectionState(BluetoothAdapter.STATE_DISCONNECTED);
            mProfileConnectionState.clear();
            invalidateGetProfileConnectionStateCache();
            mProfilesConnected = 0;
            mProfilesConnecting = 0;
            mProfilesDisconnecting = 0;
            // This keeps NV up-to date on first-boot after flash.
            setDiscoverableTimeout(mDiscoverableTimeout);
        }
    }

    void discoveryStateChangeCallback(int state) {
        infoLog("Callback:discoveryStateChangeCallback with state:" + state);
        synchronized (mObject) {
            Intent intent;
            if (state == AbstractionLayer.BT_DISCOVERY_STOPPED) {
                mDiscovering = false;
                mService.clearDiscoveringPackages();
                mDiscoveryEndMs = System.currentTimeMillis();
                intent = new Intent(BluetoothAdapter.ACTION_DISCOVERY_FINISHED);
                mService.sendBroadcast(
                        intent, BLUETOOTH_SCAN, getBroadcastOptionsForDiscoveryFinished());
            } else if (state == AbstractionLayer.BT_DISCOVERY_STARTED) {
                mDiscovering = true;
                mDiscoveryEndMs = System.currentTimeMillis() + DEFAULT_DISCOVERY_TIMEOUT_MS;
                intent = new Intent(BluetoothAdapter.ACTION_DISCOVERY_STARTED);
                mService.sendBroadcast(
                        intent, BLUETOOTH_SCAN, Utils.getTempBroadcastOptions().toBundle());
            }
        }
    }

    /**
     * @return broadcast options for ACTION_DISCOVERY_FINISHED broadcast
     */
    private static @NonNull Bundle getBroadcastOptionsForDiscoveryFinished() {
        final BroadcastOptions options = Utils.getTempBroadcastOptions();
        if (SdkLevel.isAtLeastU()) {
            options.setDeliveryGroupPolicy(BroadcastOptions.DELIVERY_GROUP_POLICY_MOST_RECENT);
            options.setDeferralPolicy(BroadcastOptions.DEFERRAL_POLICY_UNTIL_ACTIVE);
        }
        return options.toBundle();
    }

    protected void dump(FileDescriptor fd, PrintWriter writer, String[] args) {
        writer.println(TAG);
        writer.println("  " + "Name: " + getName());
        writer.println("  " + "Address: " + Utils.getRedactedAddressStringFromByte(mAddress));
        writer.println("  " + "ConnectionState: " + dumpConnectionState(getConnectionState()));
        writer.println("  " + "State: " + BluetoothAdapter.nameForState(getState()));
        writer.println("  " + "MaxConnectedAudioDevices: " + getMaxConnectedAudioDevices());
        writer.println("  " + "A2dpOffloadEnabled: " + mA2dpOffloadEnabled);
        writer.println("  " + "Discovering: " + mDiscovering);
        writer.println("  " + "DiscoveryEndMs: " + mDiscoveryEndMs);

        writer.println("  " + "Bonded devices:");
        StringBuilder sb = new StringBuilder();
        for (BluetoothDevice device : mBondedDevices) {
            String address = device.getAddress();
            String brEdrAddress =
                    Flags.identityAddressNullIfNotKnown()
                            ? Utils.getBrEdrAddress(device)
                            : mService.getIdentityAddress(address);
            if (brEdrAddress.equals(address)) {
                writer.println(
                        "    "
                                + BluetoothUtils.toAnonymizedAddress(address)
                                + " ["
                                + dumpDeviceType(mRemoteDevices.getType(device))
                                + "][ 0x"
                                + String.format("%06X", mRemoteDevices.getBluetoothClass(device))
                                + " ] "
                                + Utils.getName(device));
            } else {
                sb.append("    ")
                        .append(BluetoothUtils.toAnonymizedAddress(address))
                        .append(" => ")
                        .append(BluetoothUtils.toAnonymizedAddress(brEdrAddress))
                        .append(" [")
                        .append(dumpDeviceType(mRemoteDevices.getType(device)))
                        .append("][ 0x")
                        .append(String.format("%06X", mRemoteDevices.getBluetoothClass(device)))
                        .append(" ] ")
                        .append(Utils.getName(device))
                        .append("\n");
            }
        }
        writer.println(sb.toString());
    }

    private String dumpDeviceType(int deviceType) {
        switch (deviceType) {
            case BluetoothDevice.DEVICE_TYPE_UNKNOWN:
                return " ???? ";
            case BluetoothDevice.DEVICE_TYPE_CLASSIC:
                return "BR/EDR";
            case BluetoothDevice.DEVICE_TYPE_LE:
                return "  LE  ";
            case BluetoothDevice.DEVICE_TYPE_DUAL:
                return " DUAL ";
            default:
                return "Invalid device type: " + deviceType;
        }
    }

    private String dumpConnectionState(int state) {
        switch (state) {
            case BluetoothAdapter.STATE_DISCONNECTED:
                return "STATE_DISCONNECTED";
            case BluetoothAdapter.STATE_DISCONNECTING:
                return "STATE_DISCONNECTING";
            case BluetoothAdapter.STATE_CONNECTING:
                return "STATE_CONNECTING";
            case BluetoothAdapter.STATE_CONNECTED:
                return "STATE_CONNECTED";
            default:
                return "Unknown Connection State " + state;
        }
    }

    private static void infoLog(String msg) {
        Log.i(TAG, msg);
    }

    private static void debugLog(String msg) {
        Log.d(TAG, msg);
    }
}
