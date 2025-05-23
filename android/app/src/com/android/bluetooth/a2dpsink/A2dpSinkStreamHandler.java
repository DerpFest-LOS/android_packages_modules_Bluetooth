/*
 * Copyright (C) 2016 The Android Open Source Project
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

package com.android.bluetooth.a2dpsink;

import android.content.pm.PackageManager;
import android.media.AudioAttributes;
import android.media.AudioFocusRequest;
import android.media.AudioManager;
import android.media.AudioManager.OnAudioFocusChangeListener;
import android.media.MediaPlayer;
import android.os.Handler;
import android.os.Message;
import android.util.Log;

import com.android.bluetooth.R;
import com.android.bluetooth.avrcpcontroller.AvrcpControllerService;

/**
 * Bluetooth A2DP SINK Streaming Handler.
 *
 * <p>This handler defines how the stack behaves once the A2DP connection is established and both
 * devices are ready for streaming. For simplification we assume that the connection can either
 * stream music immediately (i.e. data packets coming in or have potential to come in) or it cannot
 * stream (i.e. Idle and Open states are treated alike). See Fig 4-1 of GAVDP Spec 1.0.
 *
 * <p>Note: There are several different audio tracks that a connected phone may like to transmit
 * over the A2DP stream including Music, Navigation, Assistant, and Notifications. Music is the only
 * track that is almost always accompanied with an AVRCP play/pause command.
 *
 * <p>Streaming is initiated by either an explicit play command from user interaction or audio
 * coming from the phone. Streaming is terminated when either the user pauses the audio, the audio
 * stream from the phone ends, the phone disconnects, or audio focus is lost. During playback if
 * there is a change to audio focus playback may be temporarily paused and then resumed when focus
 * is restored.
 */
public class A2dpSinkStreamHandler extends Handler {
    private static final String TAG = A2dpSinkStreamHandler.class.getSimpleName();

    // Configuration Variables
    private static final int DEFAULT_DUCK_PERCENT = 25;

    // Incoming events.
    public static final int SRC_STR_START = 0; // Audio stream from remote device started
    public static final int SRC_STR_STOP = 1; // Audio stream from remote device stopped
    public static final int SNK_PLAY = 2; // Play command was generated from local device
    public static final int SNK_PAUSE = 3; // Pause command was generated from local device
    public static final int SRC_PLAY = 4; // Play command was generated from remote device
    public static final int SRC_PAUSE = 5; // Pause command was generated from remote device
    public static final int DISCONNECT = 6; // Remote device was disconnected
    public static final int AUDIO_FOCUS_CHANGE = 7; // Audio focus callback with associated change
    public static final int REQUEST_FOCUS = 8; // Request focus when the media service is active

    // Used to indicate focus lost
    private static final int STATE_FOCUS_LOST = 0;
    // Used to inform bluedroid that focus is granted
    private static final int STATE_FOCUS_GRANTED = 1;

    // Private variables.
    private final A2dpSinkService mA2dpSinkService;
    private final A2dpSinkNativeInterface mNativeInterface;
    private final AudioManager mAudioManager;

    // Keep track if the remote device is providing audio
    private boolean mStreamAvailable = false;
    // Keep track of the relevant audio focus (None, Transient, Gain)
    private int mAudioFocus = AudioManager.AUDIOFOCUS_NONE;

    // In order for Bluetooth to be considered as an audio source capable of receiving media key
    // events (In the eyes of MediaSessionService), we need an active MediaPlayer in addition to a
    // MediaSession. Because of this, the media player below plays an incredibly short, silent audio
    // sample so that MediaSessionService and AudioPlaybackStateMonitor will believe that we're the
    // current active player and send the Bluetooth process media events. This allows AVRCP
    // controller to create a MediaSession and handle the events if it would like. The player and
    // session requirement is a restriction currently imposed by the media framework code and could
    // be reconsidered in the future.
    private MediaPlayer mMediaPlayer = null;

    // Focus changes when we are currently holding focus.
    private OnAudioFocusChangeListener mAudioFocusListener =
            new OnAudioFocusChangeListener() {
                @Override
                public void onAudioFocusChange(int focusChange) {
                    Log.d(TAG, "onAudioFocusChangeListener(focusChange= " + focusChange + ")");
                    A2dpSinkStreamHandler.this
                            .obtainMessage(AUDIO_FOCUS_CHANGE, focusChange)
                            .sendToTarget();
                }
            };

    public A2dpSinkStreamHandler(
            A2dpSinkService a2dpSinkService, A2dpSinkNativeInterface nativeInterface) {
        mA2dpSinkService = a2dpSinkService;
        mNativeInterface = nativeInterface;
        mAudioManager = mA2dpSinkService.getSystemService(AudioManager.class);
    }

    /** Safely clean up this stream handler object */
    public void cleanup() {
        abandonAudioFocus();
        removeCallbacksAndMessages(null);
    }

    void requestAudioFocus(boolean request) {
        obtainMessage(REQUEST_FOCUS, request).sendToTarget();
    }

    int getFocusState() {
        return mAudioFocus;
    }

    boolean isPlaying() {
        return (mStreamAvailable
                && (mAudioFocus == AudioManager.AUDIOFOCUS_GAIN
                        || mAudioFocus == AudioManager.AUDIOFOCUS_LOSS_TRANSIENT_CAN_DUCK));
    }

    @Override
    public void handleMessage(Message message) {
        Log.d(TAG, "process message: " + message.what + ", audioFocus=" + mAudioFocus);
        switch (message.what) {
            case SRC_STR_START:
                mStreamAvailable = true;
                if (isTvDevice() || shouldRequestFocus()) {
                    requestAudioFocusIfNone();
                }
                break;

            case SRC_STR_STOP:
                // Audio stream has stopped, maintain focus but stop avrcp updates.
                break;

            case SNK_PLAY:
                // Local play command, gain focus and start avrcp updates.
                requestAudioFocusIfNone();
                break;

            case SNK_PAUSE:
                mStreamAvailable = false;
                // Local pause command, maintain focus but stop avrcp updates.
                break;

            case SRC_PLAY:
                mStreamAvailable = true;
                // Remote play command.
                if (isIotDevice() || isTvDevice() || shouldRequestFocus()) {
                    requestAudioFocusIfNone();
                    break;
                }
                break;

            case SRC_PAUSE:
                mStreamAvailable = false;
                // Remote pause command, stop avrcp updates.
                break;

            case REQUEST_FOCUS:
                requestAudioFocusIfNone();
                break;

            case DISCONNECT:
                // Remote device has disconnected, restore everything to default state.
                mStreamAvailable = false;
                break;

            case AUDIO_FOCUS_CHANGE:
                final int focusChangeCode = (int) message.obj;
                Log.d(
                        TAG,
                        "New audioFocus =  "
                                + focusChangeCode
                                + " Previous audio focus = "
                                + mAudioFocus);
                mAudioFocus = focusChangeCode;
                // message.obj is the newly granted audio focus.
                switch (mAudioFocus) {
                    case AudioManager.AUDIOFOCUS_GAIN:
                        // Begin playing audio
                        startFluorideStreaming();
                        break;

                    case AudioManager.AUDIOFOCUS_LOSS_TRANSIENT_CAN_DUCK:
                        // Make the volume duck.
                        int duckPercent =
                                mA2dpSinkService
                                        .getResources()
                                        .getInteger(R.integer.a2dp_sink_duck_percent);
                        if (duckPercent < 0 || duckPercent > 100) {
                            Log.e(TAG, "Invalid duck percent using default.");
                            duckPercent = DEFAULT_DUCK_PERCENT;
                        }
                        float duckRatio = (duckPercent / 100.0f);
                        Log.d(TAG, "Setting reduce gain on transient loss gain=" + duckRatio);
                        setFluorideAudioTrackGain(duckRatio);
                        break;

                    case AudioManager.AUDIOFOCUS_LOSS_TRANSIENT:
                        // Temporary loss of focus. Set gain to zero.
                        setFluorideAudioTrackGain(0);
                        break;

                    case AudioManager.AUDIOFOCUS_LOSS:
                        // Permanent loss of focus probably due to another audio app, abandon focus
                        abandonAudioFocus();
                        break;
                }

                // Route new focus state to AVRCP Controller to handle media player states
                AvrcpControllerService avrcpControllerService =
                        AvrcpControllerService.getAvrcpControllerService();
                if (avrcpControllerService != null) {
                    avrcpControllerService.onAudioFocusStateChanged(focusChangeCode);
                } else {
                    Log.w(TAG, "AVRCP Controller Service not available to send focus events to.");
                }
                break;

            default:
                Log.w(TAG, "Received unexpected event: " + message.what);
        }
    }

    /** Utility functions. */
    private void requestAudioFocusIfNone() {
        Log.d(TAG, "requestAudioFocusIfNone()");
        if (mAudioFocus != AudioManager.AUDIOFOCUS_GAIN) {
            requestAudioFocus();
        }
    }

    private synchronized int requestAudioFocus() {
        Log.d(TAG, "requestAudioFocus()");
        // Bluetooth A2DP may carry Music, Audio Books, Navigation, or other sounds so mark content
        // type unknown.
        AudioAttributes streamAttributes =
                new AudioAttributes.Builder()
                        .setUsage(AudioAttributes.USAGE_MEDIA)
                        .setContentType(AudioAttributes.CONTENT_TYPE_UNKNOWN)
                        .build();
        // Bluetooth ducking is handled at the native layer at the request of AudioManager.
        AudioFocusRequest focusRequest =
                new AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN)
                        .setAudioAttributes(streamAttributes)
                        .setOnAudioFocusChangeListener(mAudioFocusListener, this)
                        .build();
        int focusRequestStatus = mAudioManager.requestAudioFocus(focusRequest);
        // If the request is granted begin streaming immediately and schedule an upgrade.
        if (focusRequestStatus == AudioManager.AUDIOFOCUS_REQUEST_GRANTED) {
            mAudioFocus = AudioManager.AUDIOFOCUS_GAIN;
            final Message a2dpSinkStreamHandlerMessage =
                    A2dpSinkStreamHandler.this.obtainMessage(AUDIO_FOCUS_CHANGE, mAudioFocus);
            A2dpSinkStreamHandler.this.sendMessageAtFrontOfQueue(a2dpSinkStreamHandlerMessage);
        } else {
            Log.e(TAG, "Audio focus was not granted:" + focusRequestStatus);
        }
        return focusRequestStatus;
    }

    /**
     * Plays a silent audio sample so that MediaSessionService will be aware of the fact that
     * Bluetooth is playing audio.
     *
     * <p>Creates a new MediaPlayer if one does not already exist. Repeat calls to this function are
     * safe and will result in the silent audio sample again.
     *
     * <p>This allows the MediaSession in AVRCP Controller to be routed media key events, if we've
     * chosen to use it.
     */
    private synchronized void requestMediaKeyFocus() {
        Log.d(TAG, "requestMediaKeyFocus()");
        if (mMediaPlayer == null) {
            AudioAttributes attrs =
                    new AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_MEDIA).build();

            mMediaPlayer =
                    MediaPlayer.create(
                            mA2dpSinkService,
                            R.raw.silent,
                            attrs,
                            mAudioManager.generateAudioSessionId());
            if (mMediaPlayer == null) {
                Log.e(TAG, "Failed to initialize media player. You may not get media key events");
                return;
            }

            mMediaPlayer.setLooping(false);
            mMediaPlayer.setOnErrorListener(
                    (mp, what, extra) -> {
                        Log.e(TAG, "Silent media player error: " + what + ", " + extra);
                        releaseMediaKeyFocus();
                        return false;
                    });
        }

        mMediaPlayer.start();
    }

    private synchronized void abandonAudioFocus() {
        Log.d(TAG, "abandonAudioFocus()");
        stopFluorideStreaming();
        mAudioManager.abandonAudioFocus(mAudioFocusListener);
        mAudioFocus = AudioManager.AUDIOFOCUS_NONE;
    }

    /**
     * Destroys the silent audio sample MediaPlayer, notifying MediaSessionService of the fact we're
     * no longer playing audio.
     */
    private synchronized void releaseMediaKeyFocus() {
        Log.d(TAG, "releaseMediaKeyFocus()");
        if (mMediaPlayer == null) {
            return;
        }
        mMediaPlayer.stop();
        mMediaPlayer.release();
        mMediaPlayer = null;
    }

    private void startFluorideStreaming() {
        mNativeInterface.informAudioFocusState(STATE_FOCUS_GRANTED);
        mNativeInterface.informAudioTrackGain(1.0f);
        requestMediaKeyFocus();
    }

    private void stopFluorideStreaming() {
        releaseMediaKeyFocus();
        mNativeInterface.informAudioFocusState(STATE_FOCUS_LOST);
    }

    private void setFluorideAudioTrackGain(float gain) {
        mNativeInterface.informAudioTrackGain(gain);
    }

    private boolean isIotDevice() {
        return mA2dpSinkService
                .getPackageManager()
                .hasSystemFeature(PackageManager.FEATURE_EMBEDDED);
    }

    private boolean isTvDevice() {
        return mA2dpSinkService
                .getPackageManager()
                .hasSystemFeature(PackageManager.FEATURE_LEANBACK);
    }

    private boolean shouldRequestFocus() {
        return mA2dpSinkService
                .getResources()
                .getBoolean(R.bool.a2dp_sink_automatically_request_audio_focus);
    }
}
