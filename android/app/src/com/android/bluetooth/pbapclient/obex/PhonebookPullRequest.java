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

import android.content.ContentProviderOperation;
import android.content.ContentResolver;
import android.content.Context;
import android.content.OperationApplicationException;
import android.os.RemoteException;
import android.provider.ContactsContract;
import android.util.Log;

import com.android.bluetooth.flags.Flags;
import com.android.internal.annotations.VisibleForTesting;
import com.android.vcard.VCardEntry;

import java.util.ArrayList;

public class PhonebookPullRequest extends PullRequest {
    private static final String TAG = "PhonebookPullRequest";

    @VisibleForTesting static final int MAX_OPS = 250;

    private final Context mContext;
    public boolean complete = false;

    public PhonebookPullRequest(Context context) {
        if (Flags.pbapClientStorageRefactor()) {
            Log.w(TAG, "This object should not be used. Use PbapClientContactsStorage");
        }

        mContext = context;
        path = PbapPhonebook.LOCAL_PHONEBOOK_PATH;
    }

    @Override
    public void onPullComplete() {
        if (mEntries == null) {
            Log.e(TAG, "onPullComplete entries is null.");
            return;
        }
        Log.v(TAG, "onPullComplete with " + mEntries.size() + " count.");

        try {
            ContentResolver contactsProvider = mContext.getContentResolver();
            ArrayList<ContentProviderOperation> insertOperations = new ArrayList<>();
            // Group insert operations together to minimize inter process communication and improve
            // processing time.
            for (VCardEntry e : mEntries) {
                if (Thread.currentThread().isInterrupted()) {
                    Log.e(TAG, "Interrupted durring insert.");
                    break;
                }
                int numberOfOperations = insertOperations.size();
                // Append current vcard to list of insert operations.
                e.constructInsertOperations(contactsProvider, insertOperations);
                if (insertOperations.size() >= MAX_OPS) {
                    // If we have exceded the limit to the insert operation remove the latest vcard
                    // and submit.
                    insertOperations.subList(numberOfOperations, insertOperations.size()).clear();
                    contactsProvider.applyBatch(ContactsContract.AUTHORITY, insertOperations);
                    insertOperations = e.constructInsertOperations(contactsProvider, null);
                    if (insertOperations.size() >= MAX_OPS) {
                        // Current VCard has more than 500 attributes, drop the card.
                        insertOperations.clear();
                    }
                }
            }
            if (insertOperations.size() > 0) {
                // Apply any unsubmitted vcards.
                contactsProvider.applyBatch(ContactsContract.AUTHORITY, insertOperations);
                insertOperations.clear();
            }
            Log.v(TAG, "Sync complete: add=" + mEntries.size());
        } catch (OperationApplicationException | RemoteException | NumberFormatException e) {
            Log.e(TAG, "Exception occurred while processing phonebook pull: ", e);
        } finally {
            complete = true;
        }
    }
}
