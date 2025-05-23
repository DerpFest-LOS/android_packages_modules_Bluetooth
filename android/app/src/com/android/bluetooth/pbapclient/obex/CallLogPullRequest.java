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
package com.android.bluetooth.pbapclient;

import android.accounts.Account;
import android.content.ContentProviderOperation;
import android.content.ContentValues;
import android.content.Context;
import android.content.OperationApplicationException;
import android.database.Cursor;
import android.net.Uri;
import android.os.RemoteException;
import android.provider.CallLog;
import android.provider.CallLog.Calls;
import android.provider.ContactsContract;
import android.util.Log;
import android.util.Pair;

import com.android.bluetooth.flags.Flags;
import com.android.internal.annotations.VisibleForTesting;
import com.android.vcard.VCardEntry;
import com.android.vcard.VCardEntry.PhoneData;

import java.text.ParseException;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;

public class CallLogPullRequest extends PullRequest {
    private static final String TAG = CallLogPullRequest.class.getSimpleName();

    @VisibleForTesting static final String TIMESTAMP_PROPERTY = "X-IRMC-CALL-DATETIME";
    private static final String TIMESTAMP_FORMAT = "yyyyMMdd'T'HHmmss";

    private final Account mAccount;
    private Context mContext;
    private Map<String, Integer> mCallCounter;

    public CallLogPullRequest(
            Context context, String path, Map<String, Integer> map, Account account) {
        if (Flags.pbapClientStorageRefactor()) {
            Log.w(TAG, "This object should not be used. Use PbapClientContactsStorage");
        }

        mContext = context;
        this.path = path;
        mCallCounter = map;
        mAccount = account;
    }

    @Override
    @SuppressWarnings("JavaUtilDate") // TODO: b/365629730 -- prefer Instant or LocalDate
    public void onPullComplete() {
        if (mEntries == null) {
            Log.e(TAG, "onPullComplete entries is null.");
            return;
        }

        Log.d(TAG, "onPullComplete with " + mEntries.size() + " entries");
        int type;
        try {
            if (path.equals(PbapPhonebook.ICH_PATH)) {
                type = CallLog.Calls.INCOMING_TYPE;
            } else if (path.equals(PbapPhonebook.OCH_PATH)) {
                type = CallLog.Calls.OUTGOING_TYPE;
            } else if (path.equals(PbapPhonebook.MCH_PATH)) {
                type = CallLog.Calls.MISSED_TYPE;
            } else {
                Log.w(TAG, "Unknown path type:" + path);
                return;
            }

            ArrayList<ContentProviderOperation> ops = new ArrayList<>();
            for (VCardEntry vcard : mEntries) {
                ContentValues values = new ContentValues();

                values.put(CallLog.Calls.TYPE, type);
                values.put(Calls.PHONE_ACCOUNT_ID, mAccount.name);
                List<PhoneData> phones = vcard.getPhoneList();
                if (phones == null
                        || phones.get(0).getNumber().equals(";")
                        || phones.get(0).getNumber().length() == 0) {
                    values.put(CallLog.Calls.NUMBER, "");
                } else {
                    String phoneNumber = phones.get(0).getNumber();
                    values.put(CallLog.Calls.NUMBER, phoneNumber);
                    if (mCallCounter.get(phoneNumber) != null) {
                        int updateCounter = mCallCounter.get(phoneNumber) + 1;
                        mCallCounter.put(phoneNumber, updateCounter);
                    } else {
                        mCallCounter.put(phoneNumber, 1);
                    }
                }
                List<Pair<String, String>> irmc = vcard.getUnknownXData();
                SimpleDateFormat parser = new SimpleDateFormat(TIMESTAMP_FORMAT);
                if (irmc != null) {
                    for (Pair<String, String> pair : irmc) {
                        if (pair.first.startsWith(TIMESTAMP_PROPERTY)) {
                            try {
                                values.put(CallLog.Calls.DATE, parser.parse(pair.second).getTime());
                            } catch (ParseException e) {
                                Log.d(TAG, "Failed to parse date, value=" + pair.second);
                            }
                        }
                    }
                }
                ops.add(
                        ContentProviderOperation.newInsert(CallLog.Calls.CONTENT_URI)
                                .withValues(values)
                                .withYieldAllowed(true)
                                .build());
            }
            mContext.getContentResolver().applyBatch(CallLog.AUTHORITY, ops);
            Log.d(TAG, "Updated call logs.");
            // OUTGOING_TYPE is the last callLog we fetched.
            if (type == CallLog.Calls.OUTGOING_TYPE) {
                updateTimesContacted();
            }
        } catch (RemoteException | OperationApplicationException e) {
            Log.d(TAG, "Failed to update call log for path=" + path, e);
        } finally {
            synchronized (this) {
                this.notify();
            }
        }
    }

    @VisibleForTesting
    void updateTimesContacted() {
        for (String key : mCallCounter.keySet()) {
            ContentValues values = new ContentValues();
            values.put(ContactsContract.RawContacts.TIMES_CONTACTED, mCallCounter.get(key));
            Uri uri =
                    Uri.withAppendedPath(
                            ContactsContract.PhoneLookup.CONTENT_FILTER_URI, Uri.encode(key));
            try (Cursor c = mContext.getContentResolver().query(uri, null, null, null)) {
                if (c != null && c.getCount() > 0) {
                    c.moveToNext();
                    String contactId =
                            c.getString(c.getColumnIndex(ContactsContract.PhoneLookup.CONTACT_ID));
                    Log.d(TAG, "onPullComplete: ID " + contactId + " key : " + key);
                    String where = ContactsContract.RawContacts.CONTACT_ID + "=?";
                    String[] args = new String[] {contactId};
                    mContext.getContentResolver()
                            .update(ContactsContract.RawContacts.CONTENT_URI, values, where, args);
                }
            }
        }
        Log.d(TAG, "Updated TIMES_CONTACTED");
    }
}
