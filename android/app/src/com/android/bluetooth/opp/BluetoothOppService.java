/*
 * Copyright (c) 2008-2009, Motorola, Inc.
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * - Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * - Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * - Neither the name of the Motorola, Inc. nor the names of its contributors
 * may be used to endorse or promote products derived from this software
 * without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

package com.android.bluetooth.opp;

import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothDevicePicker;
import android.bluetooth.BluetoothProfile;
import android.bluetooth.BluetoothProtoEnums;
import android.bluetooth.BluetoothSocket;
import android.bluetooth.BluetoothUtils;
import android.content.BroadcastReceiver;
import android.content.ContentResolver;
import android.content.ContentValues;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.database.CharArrayBuffer;
import android.database.ContentObserver;
import android.database.Cursor;
import android.media.MediaScannerConnection;
import android.media.MediaScannerConnection.MediaScannerConnectionClient;
import android.net.Uri;
import android.os.Binder;
import android.os.Handler;
import android.os.Message;
import android.os.Process;
import android.sysprop.BluetoothProperties;
import android.util.Log;

import com.android.bluetooth.BluetoothMethodProxy;
import com.android.bluetooth.BluetoothObexTransport;
import com.android.bluetooth.BluetoothStatsLog;
import com.android.bluetooth.IObexConnectionHandler;
import com.android.bluetooth.ObexServerSockets;
import com.android.bluetooth.Utils;
import com.android.bluetooth.btservice.AdapterService;
import com.android.bluetooth.btservice.ProfileService;
import com.android.bluetooth.content_profiles.ContentProfileErrorReportUtils;
import com.android.bluetooth.flags.Flags;
import com.android.bluetooth.sdp.SdpManagerNativeInterface;
import com.android.internal.annotations.VisibleForTesting;
import com.android.obex.ObexTransport;

import java.io.IOException;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Date;
import java.util.List;
import java.util.Locale;

/**
 * Performs the background Bluetooth OPP transfer. It also starts thread to accept incoming OPP
 * connection.
 */
// Next tag value for ContentProfileErrorReportUtils.report(): 22
public class BluetoothOppService extends ProfileService implements IObexConnectionHandler {

    /** Owned providers and activities */
    private static final String OPP_PROVIDER = BluetoothOppProvider.class.getCanonicalName();

    private static final String INCOMING_FILE_CONFIRM_ACTIVITY =
            BluetoothOppIncomingFileConfirmActivity.class.getCanonicalName();
    private static final String TRANSFER_ACTIVITY =
            BluetoothOppTransferActivity.class.getCanonicalName();
    private static final String TRANSFER_HISTORY_ACTIVITY =
            BluetoothOppTransferHistory.class.getCanonicalName();
    // Normally we would dynamically define and create these but they need to be manifest receivers
    // because they rely on explicit intents. Explicit intents don't work with dynamic receivers.
    private static final String OPP_RECEIVER = BluetoothOppReceiver.class.getCanonicalName();
    private static final String OPP_HANDOFF_RECEIVER =
            BluetoothOppHandoverReceiver.class.getCanonicalName();

    private static final byte[] SUPPORTED_OPP_FORMAT = {
        0x01 /* vCard 2.1 */,
        0x02 /* vCard 3.0 */,
        0x03 /* vCal 1.0 */,
        0x04 /* iCal 2.0 */,
        (byte) 0xFF /* Any type of object */
    };

    private class BluetoothShareContentObserver extends ContentObserver {

        BluetoothShareContentObserver() {
            super(new Handler());
        }

        @Override
        public void onChange(boolean selfChange) {
            Log.v(TAG, "ContentObserver received notification");

            // Since ContentObserver is created with Handler, onChange() can be called
            // even after the observer is unregistered.
            if (Flags.oppIgnoreContentObserverAfterServiceStop() && mObserver != this) {
                Log.d(TAG, "onChange() called after stop() is called.");
                return;
            }
            updateFromProvider();
        }
    }

    private static final String TAG = "BtOppService";

    /** Observer to get notified when the content observer's data changes */
    private BluetoothShareContentObserver mObserver;

    /** Class to handle Notification Manager updates */
    @VisibleForTesting BluetoothOppNotification mNotifier;

    private boolean mPendingUpdate;

    @VisibleForTesting UpdateThread mUpdateThread;

    private boolean mUpdateThreadRunning;

    @VisibleForTesting final List<BluetoothOppShareInfo> mShares = new ArrayList<>();
    @VisibleForTesting final List<BluetoothOppBatch> mBatches = new ArrayList<>();

    private BluetoothOppTransfer mTransfer;

    private BluetoothOppTransfer mServerTransfer;

    private int mBatchId = 1;

    /** Array used when extracting strings from content provider */
    private CharArrayBuffer mOldChars;

    /** Array used when extracting strings from content provider */
    private CharArrayBuffer mNewChars;

    private boolean mListenStarted;

    private boolean mMediaScanInProgress;

    private int mIncomingRetries;

    private ObexTransport mPendingConnection;

    private int mOppSdpHandle = -1;

    boolean mAcceptNewConnections;

    private final AdapterService mAdapterService;

    private static final String INVISIBLE =
            BluetoothShare.VISIBILITY + "=" + BluetoothShare.VISIBILITY_HIDDEN;

    private static final String WHERE_INBOUND_SUCCESS =
            BluetoothShare.DIRECTION
                    + "="
                    + BluetoothShare.DIRECTION_INBOUND
                    + " AND "
                    + BluetoothShare.STATUS
                    + "="
                    + BluetoothShare.STATUS_SUCCESS
                    + " AND "
                    + INVISIBLE;

    private static final String WHERE_CONFIRM_PENDING_INBOUND =
            BluetoothShare.DIRECTION
                    + "="
                    + BluetoothShare.DIRECTION_INBOUND
                    + " AND "
                    + BluetoothShare.USER_CONFIRMATION
                    + "="
                    + BluetoothShare.USER_CONFIRMATION_PENDING;

    @VisibleForTesting
    static final String WHERE_INVISIBLE_UNCONFIRMED =
            "("
                    + BluetoothShare.STATUS
                    + " > "
                    + BluetoothShare.STATUS_SUCCESS
                    + " AND "
                    + INVISIBLE
                    + ") OR ("
                    + WHERE_CONFIRM_PENDING_INBOUND
                    + ")";

    private static BluetoothOppService sBluetoothOppService;

    /*
     * TODO No support for queue incoming from multiple devices.
     * Make an array list of server session to support receiving queue from
     * multiple devices
     */
    private BluetoothOppObexServerSession mServerSession;

    public BluetoothOppService(AdapterService adapterService) {
        super(adapterService);

        mAdapterService = adapterService;

        IntentFilter filter = new IntentFilter(BluetoothAdapter.ACTION_STATE_CHANGED);
        filter.setPriority(IntentFilter.SYSTEM_HIGH_PRIORITY);
        registerReceiver(mBluetoothReceiver, filter);

        BluetoothOppPreference preference = BluetoothOppPreference.getInstance(this);
        if (preference != null) {
            preference.dump();
        } else {
            Log.w(TAG, "BluetoothOppPreference.getInstance returned null.");
            ContentProfileErrorReportUtils.report(
                    BluetoothProfile.OPP,
                    BluetoothProtoEnums.BLUETOOTH_OPP_SERVICE,
                    BluetoothStatsLog.BLUETOOTH_CONTENT_PROFILE_ERROR_REPORTED__TYPE__LOG_WARN,
                    0);
        }
    }

    public static boolean isEnabled() {
        return BluetoothProperties.isProfileOppEnabled().orElse(false);
    }

    @Override
    protected IProfileServiceBinder initBinder() {
        return new OppBinder();
    }

    private static class OppBinder extends Binder implements IProfileServiceBinder {

        OppBinder() {}

        @Override
        public void cleanup() {}
    }

    @Override
    public void start() {
        Log.v(TAG, "start()");

        setComponentAvailable(OPP_PROVIDER, true);
        setComponentAvailable(INCOMING_FILE_CONFIRM_ACTIVITY, true);
        setComponentAvailable(TRANSFER_ACTIVITY, true);
        setComponentAvailable(TRANSFER_HISTORY_ACTIVITY, true);
        setComponentAvailable(OPP_RECEIVER, true);
        setComponentAvailable(OPP_HANDOFF_RECEIVER, true);

        final ContentResolver contentResolver = getContentResolver();
        new Thread("trimDatabase") {
            @Override
            public void run() {
                trimDatabase(contentResolver);
            }
        }.start();

        mObserver = new BluetoothShareContentObserver();
        getContentResolver().registerContentObserver(BluetoothShare.CONTENT_URI, true, mObserver);
        mNotifier = new BluetoothOppNotification(this);
        mNotifier.cancelOppNotifications();
        updateFromProvider();
        setBluetoothOppService(this);
    }

    @Override
    public void stop() {
        if (sBluetoothOppService == null) {
            Log.w(TAG, "stop() called before start()");
            ContentProfileErrorReportUtils.report(
                    BluetoothProfile.OPP,
                    BluetoothProtoEnums.BLUETOOTH_OPP_SERVICE,
                    BluetoothStatsLog.BLUETOOTH_CONTENT_PROFILE_ERROR_REPORTED__TYPE__LOG_WARN,
                    1);
        }
        setBluetoothOppService(null);
        stopInternal();

        setComponentAvailable(OPP_PROVIDER, false);
        setComponentAvailable(INCOMING_FILE_CONFIRM_ACTIVITY, false);
        setComponentAvailable(TRANSFER_ACTIVITY, false);
        setComponentAvailable(TRANSFER_HISTORY_ACTIVITY, false);
        setComponentAvailable(OPP_RECEIVER, false);
        setComponentAvailable(OPP_HANDOFF_RECEIVER, false);
    }

    private void startListener() {
        if (!mListenStarted) {
            if (mAdapterService.isEnabled()) {
                Log.v(TAG, "Starting RfcommListener");
                mHandler.sendMessage(mHandler.obtainMessage(START_LISTENER));
                mListenStarted = true;
            }
        }
    }

    @Override
    @SuppressWarnings("JavaUtilDate") // TODO: b/365629730 -- prefer Instant or LocalDate
    public void dump(StringBuilder sb) {
        super.dump(sb);
        if (mShares.size() > 0) {
            println(sb, "Shares:");
            for (BluetoothOppShareInfo info : mShares) {
                String dir = info.mDirection == BluetoothShare.DIRECTION_OUTBOUND ? " -> " : " <- ";
                SimpleDateFormat format = new SimpleDateFormat("MM-dd HH:mm:ss", Locale.US);
                Date date = new Date(info.mTimestamp);
                println(
                        sb,
                        "  "
                                + format.format(date)
                                + dir
                                + info.mCurrentBytes
                                + "/"
                                + info.mTotalBytes);
            }
        }
    }

    /**
     * Get the current instance of {@link BluetoothOppService}
     *
     * @return current instance of {@link BluetoothOppService}
     */
    @VisibleForTesting
    public static synchronized BluetoothOppService getBluetoothOppService() {
        if (sBluetoothOppService == null) {
            Log.w(TAG, "getBluetoothOppService(): service is null");
            ContentProfileErrorReportUtils.report(
                    BluetoothProfile.OPP,
                    BluetoothProtoEnums.BLUETOOTH_OPP_SERVICE,
                    BluetoothStatsLog.BLUETOOTH_CONTENT_PROFILE_ERROR_REPORTED__TYPE__LOG_WARN,
                    2);
            return null;
        }
        if (!sBluetoothOppService.isAvailable()) {
            Log.w(TAG, "getBluetoothOppService(): service is not available");
            ContentProfileErrorReportUtils.report(
                    BluetoothProfile.OPP,
                    BluetoothProtoEnums.BLUETOOTH_OPP_SERVICE,
                    BluetoothStatsLog.BLUETOOTH_CONTENT_PROFILE_ERROR_REPORTED__TYPE__LOG_WARN,
                    3);
            return null;
        }
        return sBluetoothOppService;
    }

    private static synchronized void setBluetoothOppService(BluetoothOppService instance) {
        Log.d(TAG, "setBluetoothOppService(): set to: " + instance);
        sBluetoothOppService = instance;
    }

    private static final int START_LISTENER = 1;

    private static final int MEDIA_SCANNED = 2;

    private static final int MEDIA_SCANNED_FAILED = 3;

    private static final int MSG_INCOMING_CONNECTION_RETRY = 4;

    private static final int MSG_INCOMING_BTOPP_CONNECTION = 100;

    private static final int STOP_LISTENER = 200;

    private Handler mHandler =
            new Handler() {
                @Override
                public void handleMessage(Message msg) {
                    switch (msg.what) {
                        case STOP_LISTENER:
                            stopInternal();
                            break;
                        case START_LISTENER:
                            if (mAdapterService.isEnabled()) {
                                startSocketListener();
                            }
                            break;
                        case MEDIA_SCANNED:
                            Log.v(
                                    TAG,
                                    "Update mInfo.id "
                                            + msg.arg1
                                            + " for data uri= "
                                            + msg.obj.toString());
                            ContentValues updateValues = new ContentValues();
                            Uri contentUri = Uri.parse(BluetoothShare.CONTENT_URI + "/" + msg.arg1);
                            updateValues.put(
                                    Constants.MEDIA_SCANNED, Constants.MEDIA_SCANNED_SCANNED_OK);
                            updateValues.put(BluetoothShare.URI, msg.obj.toString()); // update
                            updateValues.put(
                                    BluetoothShare.MIMETYPE,
                                    getContentResolver().getType(Uri.parse(msg.obj.toString())));
                            getContentResolver().update(contentUri, updateValues, null, null);
                            synchronized (BluetoothOppService.this) {
                                mMediaScanInProgress = false;
                            }
                            break;
                        case MEDIA_SCANNED_FAILED:
                            Log.v(TAG, "Update mInfo.id " + msg.arg1 + " for MEDIA_SCANNED_FAILED");
                            ContentValues updateValues1 = new ContentValues();
                            Uri contentUri1 =
                                    Uri.parse(BluetoothShare.CONTENT_URI + "/" + msg.arg1);
                            updateValues1.put(
                                    Constants.MEDIA_SCANNED,
                                    Constants.MEDIA_SCANNED_SCANNED_FAILED);
                            getContentResolver().update(contentUri1, updateValues1, null, null);
                            synchronized (BluetoothOppService.this) {
                                mMediaScanInProgress = false;
                            }
                            break;
                        case MSG_INCOMING_BTOPP_CONNECTION:
                            Log.d(TAG, "Get incoming connection");
                            ObexTransport transport = (ObexTransport) msg.obj;

                            /*
                             * Strategy for incoming connections:
                             * 1. If there is no ongoing transfer, no on-hold connection, start it
                             * 2. If there is ongoing transfer, hold it for 20 seconds(1 seconds * 20 times)
                             * 3. If there is on-hold connection, reject directly
                             */
                            if (mBatches.size() == 0 && mPendingConnection == null) {
                                Log.i(TAG, "Start Obex Server");
                                createServerSession(transport);
                            } else {
                                if (mPendingConnection != null) {
                                    Log.w(TAG, "OPP busy! Reject connection");
                                    ContentProfileErrorReportUtils.report(
                                            BluetoothProfile.OPP,
                                            BluetoothProtoEnums.BLUETOOTH_OPP_SERVICE,
                                            BluetoothStatsLog
                                                    .BLUETOOTH_CONTENT_PROFILE_ERROR_REPORTED__TYPE__LOG_WARN,
                                            6);
                                    try {
                                        transport.close();
                                    } catch (IOException e) {
                                        ContentProfileErrorReportUtils.report(
                                                BluetoothProfile.OPP,
                                                BluetoothProtoEnums.BLUETOOTH_OPP_SERVICE,
                                                BluetoothStatsLog
                                                        .BLUETOOTH_CONTENT_PROFILE_ERROR_REPORTED__TYPE__EXCEPTION,
                                                7);
                                        Log.e(TAG, "close tranport error");
                                    }
                                } else {
                                    Log.i(TAG, "OPP busy! Retry after 1 second");
                                    mIncomingRetries = mIncomingRetries + 1;
                                    mPendingConnection = transport;
                                    Message msg1 = Message.obtain(mHandler);
                                    msg1.what = MSG_INCOMING_CONNECTION_RETRY;
                                    mHandler.sendMessageDelayed(msg1, 1000);
                                }
                            }
                            break;
                        case MSG_INCOMING_CONNECTION_RETRY:
                            if (mBatches.size() == 0) {
                                Log.i(TAG, "Start Obex Server");
                                createServerSession(mPendingConnection);
                                mIncomingRetries = 0;
                                mPendingConnection = null;
                            } else {
                                if (mIncomingRetries == 20) {
                                    Log.w(TAG, "Retried 20 seconds, reject connection");
                                    ContentProfileErrorReportUtils.report(
                                            BluetoothProfile.OPP,
                                            BluetoothProtoEnums.BLUETOOTH_OPP_SERVICE,
                                            BluetoothStatsLog
                                                    .BLUETOOTH_CONTENT_PROFILE_ERROR_REPORTED__TYPE__LOG_WARN,
                                            8);
                                    try {
                                        mPendingConnection.close();
                                    } catch (IOException e) {
                                        ContentProfileErrorReportUtils.report(
                                                BluetoothProfile.OPP,
                                                BluetoothProtoEnums.BLUETOOTH_OPP_SERVICE,
                                                BluetoothStatsLog
                                                        .BLUETOOTH_CONTENT_PROFILE_ERROR_REPORTED__TYPE__EXCEPTION,
                                                9);
                                        Log.e(TAG, "close transport error");
                                    }
                                    if (mServerSocket != null) {
                                        acceptNewConnections();
                                    }
                                    mIncomingRetries = 0;
                                    mPendingConnection = null;
                                } else {
                                    Log.i(TAG, "OPP busy! Retry after 1 second");
                                    mIncomingRetries = mIncomingRetries + 1;
                                    Message msg2 = Message.obtain(mHandler);
                                    msg2.what = MSG_INCOMING_CONNECTION_RETRY;
                                    mHandler.sendMessageDelayed(msg2, 1000);
                                }
                            }
                            break;
                    }
                }
            };

    private ObexServerSockets mServerSocket;

    private void startSocketListener() {
        Log.d(TAG, "start Socket Listeners");
        stopListeners();
        mServerSocket = ObexServerSockets.createInsecure(this);
        acceptNewConnections();
        SdpManagerNativeInterface nativeInterface = SdpManagerNativeInterface.getInstance();
        if (!nativeInterface.isAvailable()) {
            Log.e(TAG, "ERROR:serversocket: SdpManagerNativeInterface is not available");
            ContentProfileErrorReportUtils.report(
                    BluetoothProfile.OPP,
                    BluetoothProtoEnums.BLUETOOTH_OPP_SERVICE,
                    BluetoothStatsLog.BLUETOOTH_CONTENT_PROFILE_ERROR_REPORTED__TYPE__LOG_ERROR,
                    10);
            return;
        }
        if (mServerSocket == null) {
            Log.e(TAG, "ERROR:serversocket: mServerSocket is null");
            ContentProfileErrorReportUtils.report(
                    BluetoothProfile.OPP,
                    BluetoothProtoEnums.BLUETOOTH_OPP_SERVICE,
                    BluetoothStatsLog.BLUETOOTH_CONTENT_PROFILE_ERROR_REPORTED__TYPE__LOG_ERROR,
                    11);
            return;
        }
        mOppSdpHandle =
                nativeInterface.createOppOpsRecord(
                        "OBEX Object Push",
                        mServerSocket.getRfcommChannel(),
                        mServerSocket.getL2capPsm(),
                        0x0102,
                        SUPPORTED_OPP_FORMAT);
        Log.d(TAG, "mOppSdpHandle :" + mOppSdpHandle);
    }

    @Override
    public void cleanup() {
        Log.v(TAG, "onDestroy");

        mBatches.clear();
        mShares.clear();
        mHandler.removeCallbacksAndMessages(null);
    }

    private void unregisterReceivers() {
        try {
            if (mObserver != null) {
                getContentResolver().unregisterContentObserver(mObserver);
                mObserver = null;
            }
            unregisterReceiver(mBluetoothReceiver);
        } catch (IllegalArgumentException e) {
            ContentProfileErrorReportUtils.report(
                    BluetoothProfile.OPP,
                    BluetoothProtoEnums.BLUETOOTH_OPP_SERVICE,
                    BluetoothStatsLog.BLUETOOTH_CONTENT_PROFILE_ERROR_REPORTED__TYPE__EXCEPTION,
                    12);
            Log.w(TAG, "unregisterReceivers " + e.toString());
        }
    }

    private void stopInternal() {
        stopListeners();
        mListenStarted = false;
        // Stop Active INBOUND Transfer
        if (mServerTransfer != null) {
            mServerTransfer.onBatchCanceled();
            mServerTransfer = null;
        }
        // Stop Active OUTBOUND Transfer
        if (mTransfer != null) {
            mTransfer.onBatchCanceled();
            mTransfer = null;
        }
        unregisterReceivers();
        synchronized (BluetoothOppService.this) {
            if (mUpdateThread != null) {
                mUpdateThread.interrupt();
            }
        }
        while (mUpdateThread != null && mUpdateThreadRunning) {
            try {
                Thread.sleep(50);
            } catch (Exception e) {
                ContentProfileErrorReportUtils.report(
                        BluetoothProfile.OPP,
                        BluetoothProtoEnums.BLUETOOTH_OPP_SERVICE,
                        BluetoothStatsLog.BLUETOOTH_CONTENT_PROFILE_ERROR_REPORTED__TYPE__EXCEPTION,
                        4);
                Log.e(TAG, "Thread sleep", e);
            }
        }
        synchronized (BluetoothOppService.this) {
            if (mUpdateThread != null) {
                try {
                    mUpdateThread.join();
                } catch (InterruptedException e) {
                    ContentProfileErrorReportUtils.report(
                            BluetoothProfile.OPP,
                            BluetoothProtoEnums.BLUETOOTH_OPP_SERVICE,
                            BluetoothStatsLog
                                    .BLUETOOTH_CONTENT_PROFILE_ERROR_REPORTED__TYPE__EXCEPTION,
                            5);
                    Log.e(TAG, "Interrupted", e);
                }
                mUpdateThread = null;
            }
        }

        if (mNotifier != null) {
            mNotifier.cancelOppNotifications();
        }
    }

    /* suppose we auto accept an incoming OPUSH connection */
    private void createServerSession(ObexTransport transport) {
        mServerSession = new BluetoothOppObexServerSession(this, transport, this);
        mServerSession.preStart();
        Log.d(
                TAG,
                "Get ServerSession "
                        + mServerSession.toString()
                        + " for incoming connection"
                        + transport.toString());
    }

    private final BroadcastReceiver mBluetoothReceiver =
            new BroadcastReceiver() {
                @Override
                public void onReceive(Context context, Intent intent) {
                    String action = intent.getAction();

                    if (action.equals(BluetoothAdapter.ACTION_STATE_CHANGED)) {
                        switch (intent.getIntExtra(
                                BluetoothAdapter.EXTRA_STATE, BluetoothAdapter.ERROR)) {
                            case BluetoothAdapter.STATE_ON:
                                Log.v(TAG, "Bluetooth state changed: STATE_ON");
                                startListener();
                                // If this is within a sending process, continue the handle
                                // logic to display device picker dialog.
                                synchronized (this) {
                                    if (BluetoothOppManager.getInstance(context).mSendingFlag) {
                                        // reset the flags
                                        BluetoothOppManager.getInstance(context).mSendingFlag =
                                                false;

                                        Intent in1 =
                                                new Intent(BluetoothDevicePicker.ACTION_LAUNCH);
                                        in1.putExtra(BluetoothDevicePicker.EXTRA_NEED_AUTH, false);
                                        in1.putExtra(
                                                BluetoothDevicePicker.EXTRA_FILTER_TYPE,
                                                BluetoothDevicePicker.FILTER_TYPE_TRANSFER);
                                        in1.putExtra(
                                                BluetoothDevicePicker.EXTRA_LAUNCH_PACKAGE,
                                                getPackageName());
                                        in1.putExtra(
                                                BluetoothDevicePicker.EXTRA_LAUNCH_CLASS,
                                                BluetoothOppReceiver.class.getName());

                                        in1.setFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
                                        context.startActivity(in1);
                                    }
                                }

                                break;
                            case BluetoothAdapter.STATE_TURNING_OFF:
                                Log.v(TAG, "Bluetooth state changed: STATE_TURNING_OFF");
                                break;
                        }
                    }
                }
            };

    private void updateFromProvider() {
        synchronized (BluetoothOppService.this) {
            mPendingUpdate = true;
            if (mUpdateThread == null) {
                mUpdateThread = new UpdateThread();
                BluetoothMethodProxy.getInstance().threadStart(mUpdateThread);
                mUpdateThreadRunning = true;
            }
        }
    }

    private class UpdateThread extends Thread {
        private boolean mIsInterrupted;

        UpdateThread() {
            super("Bluetooth Share Service");
            mIsInterrupted = false;
        }

        @Override
        public void interrupt() {
            mIsInterrupted = true;
            Log.d(TAG, "OPP UpdateThread interrupted ");
            super.interrupt();
        }

        @Override
        public void run() {
            Process.setThreadPriority(Process.THREAD_PRIORITY_BACKGROUND);

            while (!mIsInterrupted) {
                synchronized (BluetoothOppService.this) {
                    if (mUpdateThread != this) {
                        mUpdateThreadRunning = false;
                        throw new IllegalStateException(
                                "multiple UpdateThreads in BluetoothOppService");
                    }
                    Log.v(
                            TAG,
                            "pendingUpdate is "
                                    + mPendingUpdate
                                    + " sListenStarted is "
                                    + mListenStarted
                                    + " isInterrupted :"
                                    + mIsInterrupted);
                    if (!mPendingUpdate) {
                        mUpdateThread = null;
                        mUpdateThreadRunning = false;
                        return;
                    }
                    mPendingUpdate = false;
                }
                Cursor cursor =
                        getContentResolver()
                                .query(
                                        BluetoothShare.CONTENT_URI,
                                        null,
                                        null,
                                        null,
                                        BluetoothShare._ID);

                if (cursor == null) {
                    mUpdateThreadRunning = false;
                    return;
                }

                cursor.moveToFirst();

                int arrayPos = 0;

                boolean isAfterLast = cursor.isAfterLast();

                int idColumn = cursor.getColumnIndexOrThrow(BluetoothShare._ID);
                /*
                 * Walk the cursor and the local array to keep them in sync. The
                 * key to the algorithm is that the ids are unique and sorted
                 * both in the cursor and in the array, so that they can be
                 * processed in order in both sources at the same time: at each
                 * step, both sources point to the lowest id that hasn't been
                 * processed from that source, and the algorithm processes the
                 * lowest id from those two possibilities. At each step: -If the
                 * array contains an entry that's not in the cursor, remove the
                 * entry, move to next entry in the array. -If the array
                 * contains an entry that's in the cursor, nothing to do, move
                 * to next cursor row and next array entry. -If the cursor
                 * contains an entry that's not in the array, insert a new entry
                 * in the array, move to next cursor row and next array entry.
                 */
                while (!isAfterLast || (arrayPos < mShares.size() && mListenStarted)) {
                    if (isAfterLast) {
                        // We're beyond the end of the cursor but there's still some
                        // stuff in the local array, which can only be junk
                        if (mShares.size() != 0) {
                            Log.v(
                                    TAG,
                                    "Array update: trimming "
                                            + mShares.get(arrayPos).mId
                                            + " @ "
                                            + arrayPos);
                        }

                        deleteShare(arrayPos); // this advances in the array
                    } else {
                        int id = cursor.getInt(idColumn);

                        if (arrayPos == mShares.size()) {
                            insertShare(cursor, arrayPos);
                            Log.v(TAG, "Array update: inserting " + id + " @ " + arrayPos);
                            ++arrayPos;
                            cursor.moveToNext();
                            isAfterLast = cursor.isAfterLast();
                        } else {
                            int arrayId = 0;
                            if (mShares.size() != 0) {
                                arrayId = mShares.get(arrayPos).mId;
                            }

                            if (arrayId < id) {
                                Log.v(TAG, "Array update: removing " + arrayId + " @ " + arrayPos);
                                deleteShare(arrayPos);
                            } else if (arrayId == id) {
                                // This cursor row already exists in the stored array.
                                updateShare(cursor, arrayPos);
                                scanFileIfNeeded(arrayPos);
                                ++arrayPos;
                                cursor.moveToNext();
                                isAfterLast = cursor.isAfterLast();
                            } else {
                                // This cursor entry didn't exist in the stored
                                // array
                                Log.v(TAG, "Array update: appending " + id + " @ " + arrayPos);
                                insertShare(cursor, arrayPos);

                                ++arrayPos;
                                cursor.moveToNext();
                                isAfterLast = cursor.isAfterLast();
                            }
                        }
                    }
                }

                mNotifier.updateNotification();

                cursor.close();
            }

            mUpdateThreadRunning = false;
        }
    }

    private void insertShare(Cursor cursor, int arrayPos) {
        String uriString = cursor.getString(cursor.getColumnIndexOrThrow(BluetoothShare.URI));
        Uri uri;
        if (uriString != null) {
            uri = Uri.parse(uriString);
            Log.d(TAG, "insertShare parsed URI: " + uri);
        } else {
            uri = null;
            Log.e(TAG, "insertShare found null URI at cursor!");
            ContentProfileErrorReportUtils.report(
                    BluetoothProfile.OPP,
                    BluetoothProtoEnums.BLUETOOTH_OPP_SERVICE,
                    BluetoothStatsLog.BLUETOOTH_CONTENT_PROFILE_ERROR_REPORTED__TYPE__LOG_ERROR,
                    13);
        }
        BluetoothOppShareInfo info =
                new BluetoothOppShareInfo(
                        cursor.getInt(cursor.getColumnIndexOrThrow(BluetoothShare._ID)),
                        uri,
                        cursor.getString(
                                cursor.getColumnIndexOrThrow(BluetoothShare.FILENAME_HINT)),
                        cursor.getString(cursor.getColumnIndexOrThrow(BluetoothShare._DATA)),
                        cursor.getString(cursor.getColumnIndexOrThrow(BluetoothShare.MIMETYPE)),
                        cursor.getInt(cursor.getColumnIndexOrThrow(BluetoothShare.DIRECTION)),
                        cursor.getString(cursor.getColumnIndexOrThrow(BluetoothShare.DESTINATION)),
                        cursor.getInt(cursor.getColumnIndexOrThrow(BluetoothShare.VISIBILITY)),
                        cursor.getInt(
                                cursor.getColumnIndexOrThrow(BluetoothShare.USER_CONFIRMATION)),
                        cursor.getInt(cursor.getColumnIndexOrThrow(BluetoothShare.STATUS)),
                        cursor.getLong(cursor.getColumnIndexOrThrow(BluetoothShare.TOTAL_BYTES)),
                        cursor.getLong(cursor.getColumnIndexOrThrow(BluetoothShare.CURRENT_BYTES)),
                        cursor.getLong(cursor.getColumnIndexOrThrow(BluetoothShare.TIMESTAMP)),
                        cursor.getInt(cursor.getColumnIndexOrThrow(Constants.MEDIA_SCANNED))
                                != Constants.MEDIA_SCANNED_NOT_SCANNED);

        Log.v(TAG, "Service adding new entry");
        Log.v(TAG, "ID      : " + info.mId);
        // Log.v(TAG, "URI     : " + ((info.mUri != null) ? "yes" : "no"));
        Log.v(TAG, "URI     : " + info.mUri);
        Log.v(TAG, "HINT    : " + info.mHint);
        Log.v(TAG, "FILENAME: " + info.mFilename);
        Log.v(TAG, "MIMETYPE: " + info.mMimetype);
        Log.v(TAG, "DIRECTION: " + info.mDirection);
        Log.v(TAG, "DESTINAT: " + info.mDestination);
        Log.v(TAG, "VISIBILI: " + info.mVisibility);
        Log.v(TAG, "CONFIRM : " + info.mConfirm);
        Log.v(TAG, "STATUS  : " + info.mStatus);
        Log.v(TAG, "TOTAL   : " + info.mTotalBytes);
        Log.v(TAG, "CURRENT : " + info.mCurrentBytes);
        Log.v(TAG, "TIMESTAMP : " + info.mTimestamp);
        Log.v(TAG, "SCANNED : " + info.mMediaScanned);

        mShares.add(arrayPos, info);

        /* Mark the info as failed if it's in invalid status */
        if (info.isObsolete()) {
            Constants.updateShareStatus(this, info.mId, BluetoothShare.STATUS_UNKNOWN_ERROR);
        }
        /*
         * Add info into a batch. The logic is
         * 1) Only add valid and readyToStart info
         * 2) If there is no batch, create a batch and insert this transfer into batch,
         * then run the batch
         * 3) If there is existing batch and timestamp match, insert transfer into batch
         * 4) If there is existing batch and timestamp does not match, create a new batch and
         * put in queue
         */

        if (info.isReadyToStart()) {
            if (info.mDirection == BluetoothShare.DIRECTION_OUTBOUND) {
                /* check if the file exists */
                BluetoothOppSendFileInfo sendFileInfo =
                        BluetoothOppUtility.getSendFileInfo(info.mUri);
                if (sendFileInfo == null || sendFileInfo.mInputStream == null) {
                    Log.e(TAG, "Can't open file for OUTBOUND info " + info.mId);
                    ContentProfileErrorReportUtils.report(
                            BluetoothProfile.OPP,
                            BluetoothProtoEnums.BLUETOOTH_OPP_SERVICE,
                            BluetoothStatsLog
                                    .BLUETOOTH_CONTENT_PROFILE_ERROR_REPORTED__TYPE__LOG_ERROR,
                            14);
                    Constants.updateShareStatus(this, info.mId, BluetoothShare.STATUS_BAD_REQUEST);
                    BluetoothOppUtility.closeSendFileInfo(info.mUri);
                    return;
                }
            }
            if (mBatches.size() == 0) {
                BluetoothOppBatch newBatch = new BluetoothOppBatch(this, info);
                newBatch.mId = mBatchId;
                mBatchId++;
                mBatches.add(newBatch);
                if (info.mDirection == BluetoothShare.DIRECTION_OUTBOUND) {
                    Log.v(
                            TAG,
                            "Service create new Batch "
                                    + newBatch.mId
                                    + " for OUTBOUND info "
                                    + info.mId);
                    mTransfer = new BluetoothOppTransfer(mAdapterService, newBatch);
                } else if (info.mDirection == BluetoothShare.DIRECTION_INBOUND) {
                    Log.v(
                            TAG,
                            "Service create new Batch "
                                    + newBatch.mId
                                    + " for INBOUND info "
                                    + info.mId);
                    mServerTransfer =
                            new BluetoothOppTransfer(mAdapterService, newBatch, mServerSession);
                }

                if (info.mDirection == BluetoothShare.DIRECTION_OUTBOUND && mTransfer != null) {
                    Log.v(
                            TAG,
                            "Service start transfer new Batch "
                                    + newBatch.mId
                                    + " for info "
                                    + info.mId);
                    mTransfer.start();
                } else if (info.mDirection == BluetoothShare.DIRECTION_INBOUND
                        && mServerTransfer != null) {
                    Log.v(
                            TAG,
                            "Service start server transfer new Batch "
                                    + newBatch.mId
                                    + " for info "
                                    + info.mId);
                    mServerTransfer.start();
                }

            } else {
                int i = findBatchWithTimeStamp(info.mTimestamp);
                if (i != -1) {
                    Log.v(
                            TAG,
                            "Service add info "
                                    + info.mId
                                    + " to existing batch "
                                    + mBatches.get(i).mId);
                    mBatches.get(i).addShare(info);
                } else {
                    // There is ongoing batch
                    BluetoothOppBatch newBatch = new BluetoothOppBatch(this, info);
                    newBatch.mId = mBatchId;
                    mBatchId++;
                    mBatches.add(newBatch);
                    Log.v(TAG, "Service add new Batch " + newBatch.mId + " for info " + info.mId);
                }
            }
        }
    }

    private void updateShare(Cursor cursor, int arrayPos) {
        BluetoothOppShareInfo info = mShares.get(arrayPos);
        int statusColumn = cursor.getColumnIndexOrThrow(BluetoothShare.STATUS);

        info.mId = cursor.getInt(cursor.getColumnIndexOrThrow(BluetoothShare._ID));
        if (info.mUri != null) {
            String uriString = stringFromCursor(info.mUri.toString(), cursor, BluetoothShare.URI);
            if (uriString != null) {
                info.mUri = Uri.parse(uriString);
            }
        } else {
            Log.w(TAG, "updateShare() called for ID " + info.mId + " with null URI");
            ContentProfileErrorReportUtils.report(
                    BluetoothProfile.OPP,
                    BluetoothProtoEnums.BLUETOOTH_OPP_SERVICE,
                    BluetoothStatsLog.BLUETOOTH_CONTENT_PROFILE_ERROR_REPORTED__TYPE__LOG_WARN,
                    15);
        }
        info.mHint = stringFromCursor(info.mHint, cursor, BluetoothShare.FILENAME_HINT);
        info.mFilename = stringFromCursor(info.mFilename, cursor, BluetoothShare._DATA);
        info.mMimetype = stringFromCursor(info.mMimetype, cursor, BluetoothShare.MIMETYPE);
        info.mDirection = cursor.getInt(cursor.getColumnIndexOrThrow(BluetoothShare.DIRECTION));
        info.mDestination = stringFromCursor(info.mDestination, cursor, BluetoothShare.DESTINATION);
        int newVisibility = cursor.getInt(cursor.getColumnIndexOrThrow(BluetoothShare.VISIBILITY));

        boolean confirmUpdated = false;
        int newConfirm =
                cursor.getInt(cursor.getColumnIndexOrThrow(BluetoothShare.USER_CONFIRMATION));

        if (info.mVisibility == BluetoothShare.VISIBILITY_VISIBLE
                && newVisibility != BluetoothShare.VISIBILITY_VISIBLE
                && (BluetoothShare.isStatusCompleted(info.mStatus)
                        || newConfirm == BluetoothShare.USER_CONFIRMATION_PENDING)) {
            mNotifier.mNotificationMgr.cancel(info.mId);
        }

        info.mVisibility = newVisibility;

        if (info.mConfirm == BluetoothShare.USER_CONFIRMATION_PENDING
                && newConfirm != BluetoothShare.USER_CONFIRMATION_PENDING) {
            confirmUpdated = true;
        }
        info.mConfirm =
                cursor.getInt(cursor.getColumnIndexOrThrow(BluetoothShare.USER_CONFIRMATION));
        int newStatus = cursor.getInt(statusColumn);

        if (BluetoothShare.isStatusCompleted(info.mStatus)) {
            mNotifier.mNotificationMgr.cancel(info.mId);
        }

        info.mStatus = newStatus;
        info.mTotalBytes = cursor.getLong(cursor.getColumnIndexOrThrow(BluetoothShare.TOTAL_BYTES));
        info.mCurrentBytes =
                cursor.getLong(cursor.getColumnIndexOrThrow(BluetoothShare.CURRENT_BYTES));
        info.mTimestamp = cursor.getLong(cursor.getColumnIndexOrThrow(BluetoothShare.TIMESTAMP));
        info.mMediaScanned =
                (cursor.getInt(cursor.getColumnIndexOrThrow(Constants.MEDIA_SCANNED))
                        != Constants.MEDIA_SCANNED_NOT_SCANNED);

        if (confirmUpdated) {
            Log.v(TAG, "Service handle info " + info.mId + " confirmation updated");
            /* Inbounds transfer user confirmation status changed, update the session server */
            int i = findBatchWithTimeStamp(info.mTimestamp);
            if (i != -1) {
                BluetoothOppBatch batch = mBatches.get(i);
                if (mServerTransfer != null && batch.mId == mServerTransfer.getBatchId()) {
                    mServerTransfer.confirmStatusChanged();
                } // TODO need to think about else
            }
        }
        int i = findBatchWithTimeStamp(info.mTimestamp);
        if (i != -1) {
            BluetoothOppBatch batch = mBatches.get(i);
            if (batch.mStatus == Constants.BATCH_STATUS_FINISHED
                    || batch.mStatus == Constants.BATCH_STATUS_FAILED) {
                Log.v(TAG, "Batch " + batch.mId + " is finished");
                if (batch.mDirection == BluetoothShare.DIRECTION_OUTBOUND) {
                    if (mTransfer == null) {
                        Log.e(TAG, "Unexpected error! mTransfer is null");
                        ContentProfileErrorReportUtils.report(
                                BluetoothProfile.OPP,
                                BluetoothProtoEnums.BLUETOOTH_OPP_SERVICE,
                                BluetoothStatsLog
                                        .BLUETOOTH_CONTENT_PROFILE_ERROR_REPORTED__TYPE__LOG_ERROR,
                                16);
                    } else if (batch.mId == mTransfer.getBatchId()) {
                        mTransfer.stop();
                    } else {
                        Log.e(
                                TAG,
                                "Unexpected error! batch id "
                                        + batch.mId
                                        + " doesn't match mTransfer id "
                                        + mTransfer.getBatchId());
                        ContentProfileErrorReportUtils.report(
                                BluetoothProfile.OPP,
                                BluetoothProtoEnums.BLUETOOTH_OPP_SERVICE,
                                BluetoothStatsLog
                                        .BLUETOOTH_CONTENT_PROFILE_ERROR_REPORTED__TYPE__LOG_ERROR,
                                17);
                    }
                    mTransfer = null;
                } else {
                    if (mServerTransfer == null) {
                        Log.e(TAG, "Unexpected error! mServerTransfer is null");
                        ContentProfileErrorReportUtils.report(
                                BluetoothProfile.OPP,
                                BluetoothProtoEnums.BLUETOOTH_OPP_SERVICE,
                                BluetoothStatsLog
                                        .BLUETOOTH_CONTENT_PROFILE_ERROR_REPORTED__TYPE__LOG_ERROR,
                                18);
                    } else if (batch.mId == mServerTransfer.getBatchId()) {
                        mServerTransfer.stop();
                    } else {
                        Log.e(
                                TAG,
                                "Unexpected error! batch id "
                                        + batch.mId
                                        + " doesn't match mServerTransfer id "
                                        + mServerTransfer.getBatchId());
                        ContentProfileErrorReportUtils.report(
                                BluetoothProfile.OPP,
                                BluetoothProtoEnums.BLUETOOTH_OPP_SERVICE,
                                BluetoothStatsLog
                                        .BLUETOOTH_CONTENT_PROFILE_ERROR_REPORTED__TYPE__LOG_ERROR,
                                19);
                    }
                    mServerTransfer = null;
                }
                removeBatch(batch);
            }
        }
    }

    /** Removes the local copy of the info about a share. */
    @VisibleForTesting
    void deleteShare(int arrayPos) {
        BluetoothOppShareInfo info = mShares.get(arrayPos);

        /*
         * Delete arrayPos from a batch. The logic is
         * 1) Search existing batch for the info
         * 2) cancel the batch
         * 3) If the batch become empty delete the batch
         */
        int i = findBatchWithTimeStamp(info.mTimestamp);
        if (i != -1) {
            BluetoothOppBatch batch = mBatches.get(i);
            if (batch.hasShare(info)) {
                Log.v(TAG, "Service cancel batch for share " + info.mId);
                batch.cancelBatch();
            }
            if (batch.isEmpty()) {
                Log.v(TAG, "Service remove batch  " + batch.mId);
                removeBatch(batch);
            }
        }
        mShares.remove(arrayPos);
    }

    private String stringFromCursor(String old, Cursor cursor, String column) {
        int index = cursor.getColumnIndexOrThrow(column);
        if (old == null) {
            return cursor.getString(index);
        }
        if (mNewChars == null) {
            mNewChars = new CharArrayBuffer(128);
        }
        cursor.copyStringToBuffer(index, mNewChars);
        int length = mNewChars.sizeCopied;
        if (length != old.length()) {
            return cursor.getString(index);
        }
        if (mOldChars == null || mOldChars.sizeCopied < length) {
            mOldChars = new CharArrayBuffer(length);
        }
        char[] oldArray = mOldChars.data;
        char[] newArray = mNewChars.data;
        old.getChars(0, length, oldArray, 0);
        for (int i = length - 1; i >= 0; --i) {
            if (oldArray[i] != newArray[i]) {
                return new String(newArray, 0, length);
            }
        }
        return old;
    }

    private int findBatchWithTimeStamp(long timestamp) {
        for (int i = mBatches.size() - 1; i >= 0; i--) {
            if (mBatches.get(i).mTimestamp == timestamp) {
                return i;
            }
        }
        return -1;
    }

    private void removeBatch(BluetoothOppBatch batch) {
        Log.v(TAG, "Remove batch " + batch.mId);
        mBatches.remove(batch);
        if (mBatches.size() > 0) {
            for (BluetoothOppBatch nextBatch : mBatches) {
                // we have a running batch
                if (nextBatch.mStatus == Constants.BATCH_STATUS_RUNNING) {
                    return;
                } else {
                    // just finish a transfer, start pending outbound transfer
                    if (nextBatch.mDirection == BluetoothShare.DIRECTION_OUTBOUND) {
                        Log.v(TAG, "Start pending outbound batch " + nextBatch.mId);
                        mTransfer = new BluetoothOppTransfer(mAdapterService, nextBatch);
                        mTransfer.start();
                        return;
                    } else if (nextBatch.mDirection == BluetoothShare.DIRECTION_INBOUND
                            && mServerSession != null) {
                        // have to support pending inbound transfer
                        // if an outbound transfer and incoming socket happens together
                        Log.v(TAG, "Start pending inbound batch " + nextBatch.mId);
                        mServerTransfer =
                                new BluetoothOppTransfer(
                                        mAdapterService, nextBatch, mServerSession);
                        mServerTransfer.start();
                        if (nextBatch.getPendingShare() != null
                                && nextBatch.getPendingShare().mConfirm
                                        == BluetoothShare.USER_CONFIRMATION_CONFIRMED) {
                            mServerTransfer.confirmStatusChanged();
                        }
                        return;
                    }
                }
            }
        }
    }

    private void scanFileIfNeeded(int arrayPos) {
        BluetoothOppShareInfo info = mShares.get(arrayPos);
        boolean isFileReceived =
                BluetoothShare.isStatusSuccess(info.mStatus)
                        && info.mDirection == BluetoothShare.DIRECTION_INBOUND
                        && !info.mMediaScanned
                        && info.mConfirm != BluetoothShare.USER_CONFIRMATION_HANDOVER_CONFIRMED;
        if (!isFileReceived) {
            return;
        }
        synchronized (BluetoothOppService.this) {
            Log.d(TAG, "Scanning file " + info.mFilename);
            if (!mMediaScanInProgress) {
                mMediaScanInProgress = true;
                new MediaScannerNotifier(this, info, mHandler);
            }
        }
    }

    // Run in a background thread at boot.
    @VisibleForTesting
    static void trimDatabase(ContentResolver contentResolver) {
        // Try-catch is important because trimDatabase can run even when the OPP_PROVIDER is
        // disabled (by OPP service, shell command, etc.).
        // At the sametime, it's ok to retry trimDatabase later when the service restart
        try {
            // remove the invisible/unconfirmed inbound shares
            int delNum =
                    BluetoothMethodProxy.getInstance()
                            .contentResolverDelete(
                                    contentResolver,
                                    BluetoothShare.CONTENT_URI,
                                    WHERE_INVISIBLE_UNCONFIRMED,
                                    null);
            Log.v(TAG, "Deleted shares, number = " + delNum);

            // Keep the latest inbound and successful shares.
            Cursor cursor =
                    BluetoothMethodProxy.getInstance()
                            .contentResolverQuery(
                                    contentResolver,
                                    BluetoothShare.CONTENT_URI,
                                    new String[] {BluetoothShare._ID},
                                    WHERE_INBOUND_SUCCESS,
                                    null,
                                    BluetoothShare._ID); // sort by id
            if (cursor == null) {
                return;
            }
            int recordNum = cursor.getCount();
            if (recordNum > Constants.MAX_RECORDS_IN_DATABASE) {
                int numToDelete = recordNum - Constants.MAX_RECORDS_IN_DATABASE;

                if (cursor.moveToPosition(numToDelete)) {
                    int columnId = cursor.getColumnIndexOrThrow(BluetoothShare._ID);
                    long id = cursor.getLong(columnId);
                    delNum =
                            BluetoothMethodProxy.getInstance()
                                    .contentResolverDelete(
                                            contentResolver,
                                            BluetoothShare.CONTENT_URI,
                                            BluetoothShare._ID + " < " + id,
                                            null);
                    Log.v(TAG, "Deleted old inbound success share: " + delNum);
                }
            }
            cursor.close();
        } catch (Exception e) {
            ContentProfileErrorReportUtils.report(
                    BluetoothProfile.OPP,
                    BluetoothProtoEnums.BLUETOOTH_OPP_SERVICE,
                    BluetoothStatsLog.BLUETOOTH_CONTENT_PROFILE_ERROR_REPORTED__TYPE__EXCEPTION,
                    20);
            Log.e(TAG, "Exception when trimming database: ", e);
        }
    }

    private static class MediaScannerNotifier implements MediaScannerConnectionClient {

        private MediaScannerConnection mConnection;

        private BluetoothOppShareInfo mInfo;

        private Context mContext;

        private Handler mCallback;

        MediaScannerNotifier(Context context, BluetoothOppShareInfo info, Handler handler) {
            mContext = context;
            mInfo = info;
            mCallback = handler;
            mConnection = new MediaScannerConnection(mContext, this);
            Log.v(TAG, "Connecting to MediaScannerConnection ");
            mConnection.connect();
        }

        @Override
        public void onMediaScannerConnected() {
            Log.v(TAG, "MediaScannerConnection onMediaScannerConnected");
            mConnection.scanFile(mInfo.mFilename, mInfo.mMimetype);
        }

        @Override
        public void onScanCompleted(String path, Uri uri) {
            try {
                Log.v(TAG, "MediaScannerConnection onScanCompleted");
                Log.v(TAG, "MediaScannerConnection path is " + path);
                Log.v(TAG, "MediaScannerConnection Uri is " + uri);
                if (uri != null) {
                    Message msg = Message.obtain();
                    msg.setTarget(mCallback);
                    msg.what = MEDIA_SCANNED;
                    msg.arg1 = mInfo.mId;
                    msg.obj = uri;
                    msg.sendToTarget();
                } else {
                    Message msg = Message.obtain();
                    msg.setTarget(mCallback);
                    msg.what = MEDIA_SCANNED_FAILED;
                    msg.arg1 = mInfo.mId;
                    msg.sendToTarget();
                }
            } catch (NullPointerException ex) {
                ContentProfileErrorReportUtils.report(
                        BluetoothProfile.OPP,
                        BluetoothProtoEnums.BLUETOOTH_OPP_SERVICE,
                        BluetoothStatsLog.BLUETOOTH_CONTENT_PROFILE_ERROR_REPORTED__TYPE__EXCEPTION,
                        21);
                Log.v(TAG, "!!!MediaScannerConnection exception: " + ex);
            } finally {
                Log.v(TAG, "MediaScannerConnection disconnect");
                mConnection.disconnect();
            }
        }
    }

    private void stopListeners() {
        SdpManagerNativeInterface nativeInterface = SdpManagerNativeInterface.getInstance();
        if (mOppSdpHandle >= 0 && nativeInterface.isAvailable()) {
            Log.d(TAG, "Removing SDP record mOppSdpHandle :" + mOppSdpHandle);
            boolean status = nativeInterface.removeSdpRecord(mOppSdpHandle);
            Log.d(TAG, "RemoveSDPrecord returns " + status);
            mOppSdpHandle = -1;
        }
        if (mServerSocket != null) {
            mServerSocket.shutdown(false);
            mServerSocket = null;
        }
        Log.d(TAG, "stopListeners: mServerSocket is null");
    }

    @Override
    public boolean onConnect(BluetoothDevice device, BluetoothSocket socket) {
        Log.d(
                TAG,
                " onConnect BluetoothSocket :"
                        + socket
                        + " \n :device :"
                        + BluetoothUtils.toAnonymizedAddress(
                                Flags.identityAddressNullIfNotKnown()
                                        ? Utils.getBrEdrAddress(device, mAdapterService)
                                        : mAdapterService.getIdentityAddress(device.getAddress())));
        if (!mAcceptNewConnections) {
            Log.d(TAG, " onConnect BluetoothSocket :" + socket + " rejected");
            return false;
        }
        BluetoothObexTransport transport = new BluetoothObexTransport(socket);
        Message msg = mHandler.obtainMessage(MSG_INCOMING_BTOPP_CONNECTION);
        msg.obj = transport;
        msg.sendToTarget();
        mAcceptNewConnections = false;
        return true;
    }

    @Override
    public void onAcceptFailed() {
        Log.d(TAG, " onAcceptFailed:");
        mHandler.sendMessage(mHandler.obtainMessage(START_LISTENER));
    }

    /** Set mAcceptNewConnections to true to allow new connections. */
    void acceptNewConnections() {
        mAcceptNewConnections = true;
    }
}
