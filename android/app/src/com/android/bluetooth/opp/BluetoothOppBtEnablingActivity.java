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

import static android.view.WindowManager.LayoutParams.SYSTEM_FLAG_HIDE_NON_SYSTEM_OVERLAY_WINDOWS;

import android.bluetooth.AlertActivity;
import android.bluetooth.BluetoothAdapter;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.os.Bundle;
import android.os.Handler;
import android.os.Message;
import android.util.Log;
import android.view.KeyEvent;
import android.view.View;
import android.widget.TextView;

import com.android.bluetooth.BluetoothMethodProxy;
import com.android.bluetooth.R;
import com.android.internal.annotations.VisibleForTesting;

/** This class is designed to show BT enabling progress. */
public class BluetoothOppBtEnablingActivity extends AlertActivity {
    private static final String TAG = "BluetoothOppEnablingActivity";

    private static final int BT_ENABLING_TIMEOUT = 0;

    @VisibleForTesting static int sBtEnablingTimeoutMs = 20000;

    private boolean mRegistered = false;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        getWindow().addSystemFlags(SYSTEM_FLAG_HIDE_NON_SYSTEM_OVERLAY_WINDOWS);
        // If BT is already enabled jus return.
        BluetoothAdapter adapter = BluetoothAdapter.getDefaultAdapter();
        if (BluetoothMethodProxy.getInstance().bluetoothAdapterIsEnabled(adapter)) {
            finish();
            return;
        }

        IntentFilter filter = new IntentFilter(BluetoothAdapter.ACTION_STATE_CHANGED);
        filter.setPriority(IntentFilter.SYSTEM_HIGH_PRIORITY);
        registerReceiver(mBluetoothReceiver, filter);
        mRegistered = true;

        mAlertBuilder.setTitle(R.string.enabling_progress_title);
        mAlertBuilder.setView(createView());
        setupAlert();

        // Add timeout for enabling progress
        mTimeoutHandler.sendMessageDelayed(
                mTimeoutHandler.obtainMessage(BT_ENABLING_TIMEOUT), sBtEnablingTimeoutMs);
    }

    private View createView() {
        View view = getLayoutInflater().inflate(R.layout.bt_enabling_progress, null);
        TextView contentView = (TextView) view.findViewById(R.id.progress_info);
        contentView.setText(getString(R.string.enabling_progress_content));

        return view;
    }

    @Override
    public boolean onKeyDown(int keyCode, KeyEvent event) {
        if (keyCode == KeyEvent.KEYCODE_BACK) {
            Log.d(TAG, "onKeyDown() called; Key: back key");
            mTimeoutHandler.removeMessages(BT_ENABLING_TIMEOUT);
            cancelSendingProgress();
        }
        return true;
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        if (mRegistered) {
            unregisterReceiver(mBluetoothReceiver);
        }
    }

    @VisibleForTesting
    final Handler mTimeoutHandler =
            new Handler() {
                @Override
                public void handleMessage(Message msg) {
                    switch (msg.what) {
                        case BT_ENABLING_TIMEOUT:
                            Log.v(TAG, "Received BT_ENABLING_TIMEOUT msg.");
                            cancelSendingProgress();
                            break;
                        default:
                            break;
                    }
                }
            };

    @VisibleForTesting
    final BroadcastReceiver mBluetoothReceiver =
            new BroadcastReceiver() {
                @Override
                public void onReceive(Context context, Intent intent) {
                    String action = intent.getAction();
                    Log.v(TAG, "Received intent: " + action);
                    if (action.equals(BluetoothAdapter.ACTION_STATE_CHANGED)) {
                        switch (intent.getIntExtra(
                                BluetoothAdapter.EXTRA_STATE, BluetoothAdapter.ERROR)) {
                            case BluetoothAdapter.STATE_ON:
                                mTimeoutHandler.removeMessages(BT_ENABLING_TIMEOUT);
                                finish();
                                break;
                            default:
                                break;
                        }
                    }
                }
            };

    private void cancelSendingProgress() {
        BluetoothOppManager oppManager = BluetoothOppManager.getInstance(this);
        if (oppManager != null && oppManager.mSendingFlag) {
            oppManager.mSendingFlag = false;
        }
        finish();
    }
}
