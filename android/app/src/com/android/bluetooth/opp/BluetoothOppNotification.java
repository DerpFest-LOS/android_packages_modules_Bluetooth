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

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.content.ContentResolver;
import android.content.Context;
import android.content.Intent;
import android.database.Cursor;
import android.graphics.drawable.Icon;
import android.net.Uri;
import android.os.Handler;
import android.os.Message;
import android.os.Process;
import android.text.format.Formatter;
import android.util.Log;

import com.android.bluetooth.BluetoothMethodProxy;
import com.android.bluetooth.R;
import com.android.bluetooth.Utils;
import com.android.bluetooth.flags.Flags;

import com.google.common.annotations.VisibleForTesting;

import java.util.HashMap;

/**
 * This class handles the updating of the Notification Manager for the cases where there is an
 * ongoing transfer, incoming transfer need confirm and complete (successful or failed) transfer.
 */
class BluetoothOppNotification {
    private static final String TAG = "BluetoothOppNotification";

    static final String STATUS = "(" + BluetoothShare.STATUS + " == '192'" + ")";

    static final String VISIBLE =
            "("
                    + BluetoothShare.VISIBILITY
                    + " IS NULL OR "
                    + BluetoothShare.VISIBILITY
                    + " == '"
                    + BluetoothShare.VISIBILITY_VISIBLE
                    + "'"
                    + ")";

    static final String CONFIRM =
            "("
                    + BluetoothShare.USER_CONFIRMATION
                    + " == '"
                    + BluetoothShare.USER_CONFIRMATION_CONFIRMED
                    + "' OR "
                    + BluetoothShare.USER_CONFIRMATION
                    + " == '"
                    + BluetoothShare.USER_CONFIRMATION_AUTO_CONFIRMED
                    + "' OR "
                    + BluetoothShare.USER_CONFIRMATION
                    + " == '"
                    + BluetoothShare.USER_CONFIRMATION_HANDOVER_CONFIRMED
                    + "'"
                    + ")";

    static final String NOT_THROUGH_HANDOVER =
            "("
                    + BluetoothShare.USER_CONFIRMATION
                    + " != '"
                    + BluetoothShare.USER_CONFIRMATION_HANDOVER_CONFIRMED
                    + "'"
                    + ")";

    static final String WHERE_RUNNING = STATUS + " AND " + VISIBLE + " AND " + CONFIRM;

    static final String WHERE_COMPLETED =
            BluetoothShare.STATUS + " >= '200' AND " + VISIBLE + " AND " + NOT_THROUGH_HANDOVER;
    // Don't show handover-initiated transfers

    static final String WHERE_COMPLETED_OUTBOUND =
            WHERE_COMPLETED
                    + " AND "
                    + "("
                    + BluetoothShare.DIRECTION
                    + " == "
                    + BluetoothShare.DIRECTION_OUTBOUND
                    + ")";

    static final String WHERE_COMPLETED_INBOUND =
            WHERE_COMPLETED
                    + " AND "
                    + "("
                    + BluetoothShare.DIRECTION
                    + " == "
                    + BluetoothShare.DIRECTION_INBOUND
                    + ")";

    private static final String WHERE_CONFIRM_PENDING =
            BluetoothShare.USER_CONFIRMATION
                    + " == '"
                    + BluetoothShare.USER_CONFIRMATION_PENDING
                    + "'"
                    + " AND "
                    + VISIBLE;

    public NotificationManager mNotificationMgr;

    private NotificationChannel mNotificationChannel;
    private static final String OPP_NOTIFICATION_CHANNEL = "opp_notification_channel";

    private Context mContext;

    private final HashMap<String, NotificationItem> mNotifications = new HashMap<>();

    @VisibleForTesting NotificationUpdateThread mUpdateNotificationThread;

    private int mPendingUpdate = 0;

    public static final int NOTIFICATION_ID_PROGRESS = -1000004;

    @VisibleForTesting static final int NOTIFICATION_ID_OUTBOUND_COMPLETE = -1000005;

    @VisibleForTesting static final int NOTIFICATION_ID_INBOUND_COMPLETE = -1000006;

    static final int NOTIFICATION_ID_COMPLETE_SUMMARY = -1000007;

    private static final String NOTIFICATION_GROUP_KEY_PROGRESS = "PROGRESS";

    private static final String NOTIFICATION_GROUP_KEY_TRANSFER_COMPLETE = "TRANSFER_COMPLETE";

    private static final String NOTIFICATION_GROUP_KEY_INCOMING_FILE_CONFIRM =
            "INCOMING_FILE_CONFIRM";

    private boolean mUpdateCompleteNotification = true;

    private ContentResolver mContentResolver = null;

    /** This inner class is used to describe some properties for one transfer. */
    static class NotificationItem {
        public int id; // This first field _id in db;

        public int direction; // to indicate sending or receiving

        public long totalCurrent = 0; // current transfer bytes

        public long totalTotal = 0; // total bytes for current transfer

        public long timeStamp = 0; // Database time stamp. Used for sorting ongoing transfers.

        public String description; // the text above progress bar

        public boolean handoverInitiated = false;
        // transfer initiated by connection handover (eg NFC)

        public String destination; // destination associated with this transfer
    }

    /**
     * Constructor
     *
     * @param ctx The context to use to obtain access to the Notification Service
     */
    BluetoothOppNotification(Context ctx) {
        mContext = ctx;
        mNotificationMgr = mContext.getSystemService(NotificationManager.class);
        mNotificationChannel =
                new NotificationChannel(
                        OPP_NOTIFICATION_CHANNEL,
                        mContext.getString(R.string.opp_notification_group),
                        NotificationManager.IMPORTANCE_HIGH);

        mNotificationMgr.createNotificationChannel(mNotificationChannel);
        // Get Content Resolver object one time
        mContentResolver = mContext.getContentResolver();
    }

    /** Update the notification ui. */
    public void updateNotification() {
        synchronized (BluetoothOppNotification.this) {
            mPendingUpdate++;
            if (mPendingUpdate > 1) {
                Log.v(TAG, "update too frequent, put in queue");
                return;
            }
            if (!mHandler.hasMessages(NOTIFY)) {
                Log.v(TAG, "send message");
                mHandler.sendMessage(mHandler.obtainMessage(NOTIFY));
            }
        }
    }

    private static final int NOTIFY = 0;
    // Use 1 second timer to limit notification frequency.
    // 1. On the first notification, create the update thread.
    //    Buffer other updates.
    // 2. Update thread will clear mPendingUpdate.
    // 3. Handler sends a delayed message to self
    // 4. Handler checks if there are any more updates after 1 second.
    // 5. If there is an update, update it else stop.
    private Handler mHandler =
            new Handler() {
                @Override
                public void handleMessage(Message msg) {
                    switch (msg.what) {
                        case NOTIFY:
                            synchronized (BluetoothOppNotification.this) {
                                if (mPendingUpdate > 0 && mUpdateNotificationThread == null) {
                                    Log.v(TAG, "new notify threadi!");
                                    mUpdateNotificationThread = new NotificationUpdateThread();
                                    mUpdateNotificationThread.start();
                                    Log.v(TAG, "send delay message");
                                    mHandler.sendMessageDelayed(
                                            mHandler.obtainMessage(NOTIFY), 1000);
                                } else if (mPendingUpdate > 0) {
                                    Log.v(TAG, "previous thread is not finished yet");
                                    mHandler.sendMessageDelayed(
                                            mHandler.obtainMessage(NOTIFY), 1000);
                                }
                                break;
                            }
                    }
                }
            };

    private class NotificationUpdateThread extends Thread {

        NotificationUpdateThread() {
            super("Notification Update Thread");
        }

        @Override
        public void run() {
            Process.setThreadPriority(Process.THREAD_PRIORITY_BACKGROUND);
            synchronized (BluetoothOppNotification.this) {
                if (mUpdateNotificationThread != this) {
                    throw new IllegalStateException(
                            "multiple UpdateThreads in BluetoothOppNotification");
                }
                mPendingUpdate = 0;
            }
            updateActiveNotification();
            updateCompletedNotification();
            updateIncomingFileConfirmNotification();
            synchronized (BluetoothOppNotification.this) {
                mUpdateNotificationThread = null;
            }
        }
    }

    @VisibleForTesting
    void updateActiveNotification() {
        // Active transfers
        Cursor cursor =
                BluetoothMethodProxy.getInstance()
                        .contentResolverQuery(
                                mContentResolver,
                                BluetoothShare.CONTENT_URI,
                                null,
                                WHERE_RUNNING,
                                null,
                                BluetoothShare._ID);
        if (cursor == null) {
            return;
        }

        // If there is active transfers, then no need to update completed transfer
        // notifications
        if (cursor.getCount() > 0) {
            mUpdateCompleteNotification = false;
        } else {
            mUpdateCompleteNotification = true;
        }
        Log.v(TAG, "mUpdateCompleteNotification = " + mUpdateCompleteNotification);

        // Collate the notifications
        final int timestampIndex = cursor.getColumnIndexOrThrow(BluetoothShare.TIMESTAMP);
        final int directionIndex = cursor.getColumnIndexOrThrow(BluetoothShare.DIRECTION);
        final int idIndex = cursor.getColumnIndexOrThrow(BluetoothShare._ID);
        final int totalBytesIndex = cursor.getColumnIndexOrThrow(BluetoothShare.TOTAL_BYTES);
        final int currentBytesIndex = cursor.getColumnIndexOrThrow(BluetoothShare.CURRENT_BYTES);
        final int dataIndex = cursor.getColumnIndexOrThrow(BluetoothShare._DATA);
        final int filenameHintIndex = cursor.getColumnIndexOrThrow(BluetoothShare.FILENAME_HINT);
        final int confirmIndex = cursor.getColumnIndexOrThrow(BluetoothShare.USER_CONFIRMATION);
        final int destinationIndex = cursor.getColumnIndexOrThrow(BluetoothShare.DESTINATION);

        mNotifications.clear();
        for (cursor.moveToFirst(); !cursor.isAfterLast(); cursor.moveToNext()) {
            long timeStamp = cursor.getLong(timestampIndex);
            int dir = cursor.getInt(directionIndex);
            int id = cursor.getInt(idIndex);
            long total = cursor.getLong(totalBytesIndex);
            long current = cursor.getLong(currentBytesIndex);
            int confirmation = cursor.getInt(confirmIndex);

            String destination = cursor.getString(destinationIndex);
            String fileName = cursor.getString(dataIndex);
            if (fileName == null) {
                fileName = cursor.getString(filenameHintIndex);
            }
            if (fileName == null) {
                fileName = mContext.getString(R.string.unknown_file);
            }

            String batchID = Long.toString(timeStamp);

            // sending objects in one batch has same timeStamp
            if (mNotifications.containsKey(batchID)) {
                // NOTE: currently no such case
                // Batch sending case
            } else {
                NotificationItem item = new NotificationItem();
                item.timeStamp = timeStamp;
                item.id = id;
                item.direction = dir;
                if (item.direction == BluetoothShare.DIRECTION_OUTBOUND) {
                    item.description = mContext.getString(R.string.notification_sending, fileName);
                } else if (item.direction == BluetoothShare.DIRECTION_INBOUND) {
                    item.description =
                            mContext.getString(R.string.notification_receiving, fileName);
                } else {
                    Log.v(TAG, "mDirection ERROR!");
                }
                item.totalCurrent = current;
                item.totalTotal = total;
                item.handoverInitiated =
                        confirmation == BluetoothShare.USER_CONFIRMATION_HANDOVER_CONFIRMED;
                item.destination = destination;
                mNotifications.put(batchID, item);

                Log.v(
                        TAG,
                        "ID="
                                + item.id
                                + "; batchID="
                                + batchID
                                + "; totoalCurrent"
                                + item.totalCurrent
                                + "; totalTotal="
                                + item.totalTotal);
            }
        }
        cursor.close();

        // Add the notifications
        for (NotificationItem item : mNotifications.values()) {
            if (item.handoverInitiated) {
                float progress = 0;
                if (item.totalTotal == -1) {
                    progress = -1;
                } else {
                    progress = (float) item.totalCurrent / item.totalTotal;
                }

                // Let NFC service deal with notifications for this transfer
                Intent intent = new Intent(Constants.ACTION_BT_OPP_TRANSFER_PROGRESS);
                if (item.direction == BluetoothShare.DIRECTION_INBOUND) {
                    intent.putExtra(
                            Constants.EXTRA_BT_OPP_TRANSFER_DIRECTION,
                            Constants.DIRECTION_BLUETOOTH_INCOMING);
                } else {
                    intent.putExtra(
                            Constants.EXTRA_BT_OPP_TRANSFER_DIRECTION,
                            Constants.DIRECTION_BLUETOOTH_OUTGOING);
                }
                intent.putExtra(Constants.EXTRA_BT_OPP_TRANSFER_ID, item.id);
                intent.putExtra(Constants.EXTRA_BT_OPP_TRANSFER_PROGRESS, progress);
                intent.putExtra(Constants.EXTRA_BT_OPP_ADDRESS, item.destination);
                mContext.sendBroadcast(
                        intent,
                        Constants.HANDOVER_STATUS_PERMISSION,
                        Utils.getTempBroadcastOptions().toBundle());
                continue;
            }
            // Build the notification object
            // TODO: split description into two rows with filename in second row
            Notification.Builder b = new Notification.Builder(mContext, OPP_NOTIFICATION_CHANNEL);
            b.setOnlyAlertOnce(true);
            b.setColor(
                    mContext.getResources()
                            .getColor(
                                    android.R.color.system_notification_accent_color,
                                    mContext.getTheme()));
            b.setContentTitle(item.description);
            b.setSubText(
                    BluetoothOppUtility.formatProgressText(item.totalTotal, item.totalCurrent));
            if (item.totalTotal != 0) {
                Log.v(
                        TAG,
                        "mCurrentBytes: "
                                + item.totalCurrent
                                + " mTotalBytes: "
                                + item.totalTotal
                                + " ("
                                + (int) ((item.totalCurrent * 100) / item.totalTotal)
                                + " %)");
                b.setProgress(
                        100,
                        (int) ((item.totalCurrent * 100) / item.totalTotal),
                        item.totalTotal == -1);
            } else {
                b.setProgress(100, 100, item.totalTotal == -1);
            }
            b.setWhen(item.timeStamp);
            if (item.direction == BluetoothShare.DIRECTION_OUTBOUND) {
                b.setSmallIcon(android.R.drawable.stat_sys_upload);
            } else if (item.direction == BluetoothShare.DIRECTION_INBOUND) {
                b.setSmallIcon(android.R.drawable.stat_sys_download);
            } else {
                Log.v(TAG, "mDirection ERROR!");
            }
            b.setOngoing(true);
            b.setLocalOnly(true);

            Intent intent = new Intent(Constants.ACTION_LIST);
            intent.setClassName(mContext, BluetoothOppReceiver.class.getName());
            intent.setDataAndNormalize(Uri.parse(BluetoothShare.CONTENT_URI + "/" + item.id));
            b.setContentIntent(
                    PendingIntent.getBroadcast(mContext, 0, intent, PendingIntent.FLAG_IMMUTABLE));
            if (Flags.oppFixMultipleNotificationsIssues()) {
                b.setGroup(NOTIFICATION_GROUP_KEY_PROGRESS);
            }
            mNotificationMgr.notify(NOTIFICATION_ID_PROGRESS, b.build());
        }
    }

    @VisibleForTesting
    void updateCompletedNotification() {
        long timeStamp = 0;
        int outboundSuccNumber = 0;
        int outboundFailNumber = 0;
        int outboundNum;
        int inboundNum;
        int inboundSuccNumber = 0;
        int inboundFailNumber = 0;

        // Creating outbound notification
        Cursor cursor =
                BluetoothMethodProxy.getInstance()
                        .contentResolverQuery(
                                mContentResolver,
                                BluetoothShare.CONTENT_URI,
                                null,
                                WHERE_COMPLETED_OUTBOUND,
                                null,
                                BluetoothShare.TIMESTAMP + " DESC");
        if (cursor == null) {
            return;
        }

        final int timestampIndex = cursor.getColumnIndexOrThrow(BluetoothShare.TIMESTAMP);
        final int statusIndex = cursor.getColumnIndexOrThrow(BluetoothShare.STATUS);

        for (cursor.moveToFirst(); !cursor.isAfterLast(); cursor.moveToNext()) {
            if (cursor.isFirst()) {
                // Display the time for the latest transfer
                timeStamp = cursor.getLong(timestampIndex);
            }
            int status = cursor.getInt(statusIndex);

            if (BluetoothShare.isStatusError(status)) {
                outboundFailNumber++;
            } else {
                outboundSuccNumber++;
            }
        }
        Log.v(TAG, "outbound: succ-" + outboundSuccNumber + "  fail-" + outboundFailNumber);
        cursor.close();

        outboundNum = outboundSuccNumber + outboundFailNumber;
        // create the outbound notification
        if (outboundNum > 0) {
            String caption =
                    BluetoothOppUtility.formatResultText(
                            outboundSuccNumber, outboundFailNumber, mContext);

            PendingIntent pi;
            if (Flags.oppStartActivityDirectlyFromNotification()) {
                Intent in = new Intent(Constants.ACTION_OPEN_OUTBOUND_TRANSFER);
                in.setClass(mContext, BluetoothOppTransferHistory.class);
                in.setFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TOP);
                pi = PendingIntent.getActivity(mContext, 0, in, PendingIntent.FLAG_IMMUTABLE);
            } else {
                Intent in =
                        new Intent(Constants.ACTION_OPEN_OUTBOUND_TRANSFER)
                                .setClassName(mContext, BluetoothOppReceiver.class.getName());
                pi = PendingIntent.getBroadcast(mContext, 0, in, PendingIntent.FLAG_IMMUTABLE);
            }

            Intent deleteIntent = new Intent(mContext, BluetoothOppReceiver.class);
            if (Flags.oppFixMultipleNotificationsIssues()) {
                deleteIntent.setAction(Constants.ACTION_HIDE_COMPLETED_OUTBOUND_TRANSFER);
            } else {
                deleteIntent.setAction(Constants.ACTION_COMPLETE_HIDE);
            }

            Notification.Builder b =
                    new Notification.Builder(mContext, OPP_NOTIFICATION_CHANNEL)
                            .setOnlyAlertOnce(true)
                            .setContentTitle(mContext.getString(R.string.outbound_noti_title))
                            .setContentText(caption)
                            .setSmallIcon(android.R.drawable.stat_sys_upload_done)
                            .setColor(
                                    mContext.getResources()
                                            .getColor(
                                                    android.R.color
                                                            .system_notification_accent_color,
                                                    mContext.getTheme()))
                            .setContentIntent(pi)
                            .setDeleteIntent(
                                    PendingIntent.getBroadcast(
                                            mContext,
                                            0,
                                            deleteIntent,
                                            PendingIntent.FLAG_IMMUTABLE))
                            .setWhen(timeStamp)
                            .setLocalOnly(true);
            if (Flags.oppFixMultipleNotificationsIssues()) {
                b.setGroup(NOTIFICATION_GROUP_KEY_TRANSFER_COMPLETE);
            }
            mNotificationMgr.notify(NOTIFICATION_ID_OUTBOUND_COMPLETE, b.build());
        } else {
            if (mNotificationMgr != null) {
                mNotificationMgr.cancel(NOTIFICATION_ID_OUTBOUND_COMPLETE);
                Log.v(TAG, "outbound notification was removed.");
            }
        }

        // Creating inbound notification
        cursor =
                BluetoothMethodProxy.getInstance()
                        .contentResolverQuery(
                                mContentResolver,
                                BluetoothShare.CONTENT_URI,
                                null,
                                WHERE_COMPLETED_INBOUND,
                                null,
                                BluetoothShare.TIMESTAMP + " DESC");
        if (cursor == null) {
            return;
        }

        for (cursor.moveToFirst(); !cursor.isAfterLast(); cursor.moveToNext()) {
            if (cursor.isFirst()) {
                // Display the time for the latest transfer
                timeStamp = cursor.getLong(timestampIndex);
            }
            int status = cursor.getInt(statusIndex);

            if (BluetoothShare.isStatusError(status)) {
                inboundFailNumber++;
            } else {
                inboundSuccNumber++;
            }
        }
        Log.v(TAG, "inbound: succ-" + inboundSuccNumber + "  fail-" + inboundFailNumber);
        cursor.close();

        inboundNum = inboundSuccNumber + inboundFailNumber;
        // create the inbound notification
        if (inboundNum > 0) {
            String caption =
                    BluetoothOppUtility.formatResultText(
                            inboundSuccNumber, inboundFailNumber, mContext);

            PendingIntent pi;
            if (Flags.oppStartActivityDirectlyFromNotification()) {
                Intent in = new Intent(Constants.ACTION_OPEN_INBOUND_TRANSFER);
                in.setClass(mContext, BluetoothOppTransferHistory.class);
                in.setFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TOP);
                pi = PendingIntent.getActivity(mContext, 0, in, PendingIntent.FLAG_IMMUTABLE);
            } else {
                Intent in =
                        new Intent(Constants.ACTION_OPEN_INBOUND_TRANSFER)
                                .setClassName(mContext, BluetoothOppReceiver.class.getName());
                pi = PendingIntent.getBroadcast(mContext, 0, in, PendingIntent.FLAG_IMMUTABLE);
            }

            Intent deleteIntent = new Intent(mContext, BluetoothOppReceiver.class);
            if (Flags.oppFixMultipleNotificationsIssues()) {
                deleteIntent.setAction(Constants.ACTION_HIDE_COMPLETED_INBOUND_TRANSFER);
            } else {
                deleteIntent.setAction(Constants.ACTION_COMPLETE_HIDE);
            }

            Notification.Builder b =
                    new Notification.Builder(mContext, OPP_NOTIFICATION_CHANNEL)
                            .setOnlyAlertOnce(true)
                            .setContentTitle(mContext.getString(R.string.inbound_noti_title))
                            .setContentText(caption)
                            .setSmallIcon(android.R.drawable.stat_sys_download_done)
                            .setColor(
                                    mContext.getResources()
                                            .getColor(
                                                    android.R.color
                                                            .system_notification_accent_color,
                                                    mContext.getTheme()))
                            .setContentIntent(pi)
                            .setDeleteIntent(
                                    PendingIntent.getBroadcast(
                                            mContext,
                                            0,
                                            deleteIntent,
                                            PendingIntent.FLAG_IMMUTABLE))
                            .setWhen(timeStamp)
                            .setLocalOnly(true);
            if (Flags.oppFixMultipleNotificationsIssues()) {
                b.setGroup(NOTIFICATION_GROUP_KEY_TRANSFER_COMPLETE);
            }
            mNotificationMgr.notify(NOTIFICATION_ID_INBOUND_COMPLETE, b.build());
        } else {
            if (mNotificationMgr != null) {
                mNotificationMgr.cancel(NOTIFICATION_ID_INBOUND_COMPLETE);
                Log.v(TAG, "inbound notification was removed.");
            }
        }

        if (Flags.oppFixMultipleNotificationsIssues() && inboundNum > 0 && outboundNum > 0) {
            Notification.Builder b =
                    new Notification.Builder(mContext, OPP_NOTIFICATION_CHANNEL)
                            .setGroup(NOTIFICATION_GROUP_KEY_TRANSFER_COMPLETE)
                            .setGroupSummary(true)
                            .setGroupAlertBehavior(Notification.GROUP_ALERT_CHILDREN)
                            .setSmallIcon(R.drawable.ic_bluetooth_file_transfer_notification)
                            .setColor(
                                    mContext.getResources()
                                            .getColor(
                                                    android.R.color
                                                            .system_notification_accent_color,
                                                    mContext.getTheme()))
                            .setLocalOnly(true);

            mNotificationMgr.notify(NOTIFICATION_ID_COMPLETE_SUMMARY, b.build());
        }
    }

    @VisibleForTesting
    void updateIncomingFileConfirmNotification() {
        Cursor cursor =
                BluetoothMethodProxy.getInstance()
                        .contentResolverQuery(
                                mContentResolver,
                                BluetoothShare.CONTENT_URI,
                                null,
                                WHERE_CONFIRM_PENDING,
                                null,
                                BluetoothShare._ID);

        if (cursor == null) {
            return;
        }

        for (cursor.moveToFirst(); !cursor.isAfterLast(); cursor.moveToNext()) {
            BluetoothOppTransferInfo info = new BluetoothOppTransferInfo();
            BluetoothOppUtility.fillRecord(mContext, cursor, info);
            Uri contentUri = Uri.parse(BluetoothShare.CONTENT_URI + "/" + info.mID);
            String fileNameSafe = info.mFileName.replaceAll("\\s", "_");
            Intent baseIntent =
                    new Intent()
                            .setDataAndNormalize(contentUri)
                            .setClassName(mContext, BluetoothOppReceiver.class.getName());
            Notification.Action actionDecline =
                    new Notification.Action.Builder(
                                    Icon.createWithResource(mContext, R.drawable.ic_decline),
                                    mContext.getText(R.string.incoming_file_confirm_cancel),
                                    PendingIntent.getBroadcast(
                                            mContext,
                                            0,
                                            new Intent(baseIntent)
                                                    .setAction(Constants.ACTION_DECLINE),
                                            PendingIntent.FLAG_IMMUTABLE))
                            .build();
            Notification.Action actionAccept =
                    new Notification.Action.Builder(
                                    Icon.createWithResource(mContext, R.drawable.ic_accept),
                                    mContext.getText(R.string.incoming_file_confirm_ok),
                                    PendingIntent.getBroadcast(
                                            mContext,
                                            0,
                                            new Intent(baseIntent)
                                                    .setAction(Constants.ACTION_ACCEPT),
                                            PendingIntent.FLAG_IMMUTABLE))
                            .build();

            PendingIntent contentIntent;
            if (Flags.oppStartActivityDirectlyFromNotification()) {
                Intent intent = new Intent(mContext, BluetoothOppIncomingFileConfirmActivity.class);
                intent.setFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
                intent.setDataAndNormalize(contentUri);
                contentIntent =
                        PendingIntent.getActivity(
                                mContext, 0, intent, PendingIntent.FLAG_IMMUTABLE);
            } else {
                contentIntent =
                        PendingIntent.getBroadcast(
                                mContext,
                                0,
                                new Intent(baseIntent)
                                        .setAction(Constants.ACTION_INCOMING_FILE_CONFIRM),
                                PendingIntent.FLAG_IMMUTABLE);
            }

            Notification.Builder publicNotificationBuilder =
                    new Notification.Builder(mContext, OPP_NOTIFICATION_CHANNEL)
                            .setOnlyAlertOnce(true)
                            .setOngoing(true)
                            .setWhen(info.mTimeStamp)
                            .setContentIntent(contentIntent)
                            .setDeleteIntent(
                                    PendingIntent.getBroadcast(
                                            mContext,
                                            0,
                                            new Intent(baseIntent).setAction(Constants.ACTION_HIDE),
                                            PendingIntent.FLAG_IMMUTABLE))
                            .setColor(
                                    mContext.getResources()
                                            .getColor(
                                                    android.R.color
                                                            .system_notification_accent_color,
                                                    mContext.getTheme()))
                            .setContentTitle(
                                    mContext.getText(
                                            R.string.incoming_file_confirm_Notification_title))
                            .setContentText(fileNameSafe)
                            .setStyle(
                                    new Notification.BigTextStyle()
                                            .bigText(
                                                    mContext.getString(
                                                            R.string
                                                                    .incoming_file_confirm_Notification_content,
                                                            info.mDeviceName,
                                                            fileNameSafe)))
                            .setSubText(Formatter.formatFileSize(mContext, info.mTotalBytes))
                            .setSmallIcon(R.drawable.ic_bluetooth_file_transfer_notification)
                            .setLocalOnly(true);
            if (Flags.oppFixMultipleNotificationsIssues()) {
                publicNotificationBuilder.setGroup(NOTIFICATION_GROUP_KEY_INCOMING_FILE_CONFIRM);
            }

            Notification.Builder builder =
                    new Notification.Builder(mContext, OPP_NOTIFICATION_CHANNEL)
                            .setOnlyAlertOnce(true)
                            .setOngoing(true)
                            .setWhen(info.mTimeStamp)
                            .setContentIntent(contentIntent)
                            .setDeleteIntent(
                                    PendingIntent.getBroadcast(
                                            mContext,
                                            0,
                                            new Intent(baseIntent).setAction(Constants.ACTION_HIDE),
                                            PendingIntent.FLAG_IMMUTABLE))
                            .setColor(
                                    mContext.getResources()
                                            .getColor(
                                                    android.R.color
                                                            .system_notification_accent_color,
                                                    mContext.getTheme()))
                            .setContentTitle(
                                    mContext.getText(
                                            R.string.incoming_file_confirm_Notification_title))
                            .setContentText(fileNameSafe)
                            .setStyle(
                                    new Notification.BigTextStyle()
                                            .bigText(
                                                    mContext.getString(
                                                            R.string
                                                                    .incoming_file_confirm_Notification_content,
                                                            info.mDeviceName,
                                                            fileNameSafe)))
                            .setSubText(Formatter.formatFileSize(mContext, info.mTotalBytes))
                            .setSmallIcon(R.drawable.ic_bluetooth_file_transfer_notification)
                            .setLocalOnly(true)
                            .setVisibility(Notification.VISIBILITY_PRIVATE)
                            .addAction(actionDecline)
                            .addAction(actionAccept)
                            .setPublicVersion(publicNotificationBuilder.build());
            if (Flags.oppFixMultipleNotificationsIssues()) {
                builder.setGroup(NOTIFICATION_GROUP_KEY_INCOMING_FILE_CONFIRM);
            }
            mNotificationMgr.notify(NOTIFICATION_ID_PROGRESS, builder.build());
        }
        cursor.close();
    }

    void cancelOppNotifications() {
        Log.v(TAG, "cancelOppNotifications ");
        mHandler.removeCallbacksAndMessages(null);
        mNotificationMgr.cancel(NOTIFICATION_ID_PROGRESS);
        mNotificationMgr.cancel(NOTIFICATION_ID_OUTBOUND_COMPLETE);
        mNotificationMgr.cancel(NOTIFICATION_ID_INBOUND_COMPLETE);
        mNotificationMgr.cancel(NOTIFICATION_ID_COMPLETE_SUMMARY);
    }
}
