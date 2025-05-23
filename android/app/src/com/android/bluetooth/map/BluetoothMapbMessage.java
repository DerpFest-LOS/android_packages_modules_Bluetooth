/*
 * Copyright (C) 2013 Samsung System LSI
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
package com.android.bluetooth.map;

import android.bluetooth.BluetoothProfile;
import android.bluetooth.BluetoothProtoEnums;
import android.telephony.PhoneNumberUtils;
import android.util.Log;

import com.android.bluetooth.BluetoothStatsLog;
import com.android.bluetooth.content_profiles.ContentProfileErrorReportUtils;
import com.android.bluetooth.map.BluetoothMapUtils.TYPE;
import com.android.internal.annotations.VisibleForTesting;

import com.google.common.base.Ascii;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.regex.Pattern;

// Next tag value for ContentProfileErrorReportUtils.report(): 10
public abstract class BluetoothMapbMessage {

    protected static final String TAG = BluetoothMapbMessage.class.getSimpleName();

    private static final Pattern UNESCAPE_COLON = Pattern.compile("[^\\\\]:");
    protected static final Pattern COLON = Pattern.compile(":");

    private String mVersionString = "VERSION:1.0";

    public static final int INVALID_VALUE = -1;

    protected int mAppParamCharset = BluetoothMapAppParams.INVALID_VALUE_PARAMETER;

    /* BMSG attributes */
    private String mStatus = null; // READ/UNREAD
    protected TYPE mType = null; // SMS/MMS/EMAIL

    private String mFolder = null;

    /* BBODY attributes */
    protected String mEncoding = null;
    protected String mCharset = null;

    private int mBMsgLength = INVALID_VALUE;

    private List<VCard> mOriginator = null;
    private List<VCard> mRecipient = null;

    public static class VCard {
        /* VCARD attributes */
        private String mVersion;
        private String mName = null;
        private String mFormattedName = null;
        private String[] mPhoneNumbers = {};
        private String[] mEmailAddresses = {};
        private int mEnvLevel = 0;
        private String[] mBtUcis = {};
        private String[] mBtUids = {};

        /**
         * Construct a version 3.0 vCard
         *
         * @param name Structured
         * @param formattedName Formatted name
         * @param phoneNumbers a String[] of phone numbers
         * @param emailAddresses a String[] of email addresses
         * @param envLevel the bmessage envelope level (0 is the top/most outer level)
         */
        public VCard(
                String name,
                String formattedName,
                String[] phoneNumbers,
                String[] emailAddresses,
                int envLevel) {
            this.mEnvLevel = envLevel;
            this.mVersion = "3.0";
            this.mName = name != null ? name : "";
            this.mFormattedName = formattedName != null ? formattedName : "";
            setPhoneNumbers(phoneNumbers);
            if (emailAddresses != null) {
                this.mEmailAddresses = emailAddresses;
            }
        }

        /**
         * Construct a version 2.1 vCard
         *
         * @param name Structured name
         * @param phoneNumbers a String[] of phone numbers
         * @param emailAddresses a String[] of email addresses
         * @param envLevel the bmessage envelope level (0 is the top/most outer level)
         */
        public VCard(String name, String[] phoneNumbers, String[] emailAddresses, int envLevel) {
            this.mEnvLevel = envLevel;
            this.mVersion = "2.1";
            this.mName = name != null ? name : "";
            setPhoneNumbers(phoneNumbers);
            if (emailAddresses != null) {
                this.mEmailAddresses = emailAddresses;
            }
        }

        /**
         * Construct a version 3.0 vCard
         *
         * @param name Structured name
         * @param formattedName Formatted name
         * @param phoneNumbers a String[] of phone numbers
         * @param emailAddresses a String[] of email addresses if available, else null
         * @param btUids a String[] of X-BT-UIDs if available, else null
         * @param btUcis a String[] of X-BT-UCIs if available, else null
         */
        public VCard(
                String name,
                String formattedName,
                String[] phoneNumbers,
                String[] emailAddresses,
                String[] btUids,
                String[] btUcis) {
            this.mVersion = "3.0";
            this.mName = (name != null) ? name : "";
            this.mFormattedName = (formattedName != null) ? formattedName : "";
            setPhoneNumbers(phoneNumbers);
            if (emailAddresses != null) {
                this.mEmailAddresses = emailAddresses;
            }
            if (btUcis != null) {
                this.mBtUcis = btUcis;
            }
        }

        /**
         * Construct a version 2.1 vCard
         *
         * @param name Structured Name
         * @param phoneNumbers a String[] of phone numbers
         * @param emailAddresses a String[] of email addresses
         */
        public VCard(String name, String[] phoneNumbers, String[] emailAddresses) {
            this.mVersion = "2.1";
            this.mName = name != null ? name : "";
            setPhoneNumbers(phoneNumbers);
            if (emailAddresses != null) {
                this.mEmailAddresses = emailAddresses;
            }
        }

        private void setPhoneNumbers(String[] numbers) {
            if (numbers != null && numbers.length > 0) {
                mPhoneNumbers = new String[numbers.length];
                for (int i = 0, n = numbers.length; i < n; i++) {
                    String networkNumber = PhoneNumberUtils.extractNetworkPortion(numbers[i]);
                    /* extractNetworkPortion can return N if the number is a service
                     * "number" = a string with the a name in (i.e. "Some-Tele-company" would
                     * return N because of the N in compaNy)
                     * Hence we need to check if the number is actually a string with alpha chars.
                     * */
                    String strippedNumber = PhoneNumberUtils.stripSeparators(numbers[i]);
                    Boolean alpha = false;
                    if (strippedNumber != null) {
                        alpha = strippedNumber.matches("[0-9]*[a-zA-Z]+[0-9]*");
                    }
                    if (networkNumber != null && networkNumber.length() > 1 && !alpha) {
                        mPhoneNumbers[i] = networkNumber;
                    } else {
                        mPhoneNumbers[i] = numbers[i];
                    }
                }
            }
        }

        public String getFirstPhoneNumber() {
            if (mPhoneNumbers.length > 0) {
                return mPhoneNumbers[0];
            } else {
                return null;
            }
        }

        public int getEnvLevel() {
            return mEnvLevel;
        }

        public String getName() {
            return mName;
        }

        public String getFirstEmail() {
            if (mEmailAddresses.length > 0) {
                return mEmailAddresses[0];
            } else {
                return null;
            }
        }

        public String getFirstBtUci() {
            if (mBtUcis.length > 0) {
                return mBtUcis[0];
            } else {
                return null;
            }
        }

        public String getFirstBtUid() {
            if (mBtUids.length > 0) {
                return mBtUids[0];
            } else {
                return null;
            }
        }

        public void encode(StringBuilder sb) {
            sb.append("BEGIN:VCARD").append("\r\n");
            sb.append("VERSION:").append(mVersion).append("\r\n");
            if (mVersion.equals("3.0") && mFormattedName != null) {
                sb.append("FN:").append(mFormattedName).append("\r\n");
            }
            if (mName != null) {
                sb.append("N:").append(mName).append("\r\n");
            }
            for (String phoneNumber : mPhoneNumbers) {
                sb.append("TEL:").append(phoneNumber).append("\r\n");
            }
            for (String emailAddress : mEmailAddresses) {
                sb.append("EMAIL:").append(emailAddress).append("\r\n");
            }
            for (String btUid : mBtUids) {
                sb.append("X-BT-UID:").append(btUid).append("\r\n");
            }
            for (String btUci : mBtUcis) {
                sb.append("X-BT-UCI:").append(btUci).append("\r\n");
            }
            sb.append("END:VCARD").append("\r\n");
        }

        /**
         * Parse a vCard from a BMgsReader, where a line containing "BEGIN:VCARD" have just been
         * read.
         */
        static VCard parseVcard(BMsgReader reader, int envLevel) {
            String formattedName = null;
            String name = null;
            List<String> phoneNumbers = null;
            List<String> emailAddresses = null;
            List<String> btUids = null;
            List<String> btUcis = null;
            String[] parts;
            String line = reader.getLineEnforce();

            while (!line.contains("END:VCARD")) {
                line = line.trim();
                if (line.startsWith("N:")) {
                    parts = UNESCAPE_COLON.split(line);
                    if (parts.length == 2) {
                        name = parts[1];
                    } else {
                        name = "";
                    }
                } else if (line.startsWith("FN:")) {
                    parts = UNESCAPE_COLON.split(line);
                    if (parts.length == 2) {
                        formattedName = parts[1];
                    } else {
                        formattedName = "";
                    }
                } else if (line.startsWith("TEL:")) {
                    parts = UNESCAPE_COLON.split(line);
                    if (parts.length == 2) {
                        String[] subParts = UNESCAPE_COLON.split(parts[1]);
                        if (phoneNumbers == null) {
                            phoneNumbers = new ArrayList<>(1);
                        }
                        // only keep actual phone number
                        phoneNumbers.add(subParts[subParts.length - 1]);
                    }
                    // Empty phone number - ignore
                } else if (line.startsWith("EMAIL:")) {
                    parts = UNESCAPE_COLON.split(line);
                    if (parts.length == 2) {
                        String[] subParts = UNESCAPE_COLON.split(parts[1]);
                        if (emailAddresses == null) {
                            emailAddresses = new ArrayList<String>(1);
                        }
                        // only keep actual email address
                        emailAddresses.add(subParts[subParts.length - 1]);
                    }
                    // Empty email address entry - ignore
                } else if (line.startsWith("X-BT-UCI:")) {
                    parts = UNESCAPE_COLON.split(line);
                    if (parts.length == 2) {
                        String[] subParts = UNESCAPE_COLON.split(parts[1]);
                        if (btUcis == null) {
                            btUcis = new ArrayList<String>(1);
                        }
                        btUcis.add(subParts[subParts.length - 1]); // only keep actual UCI
                    }
                    // Empty UCIentry - ignore
                } else if (line.startsWith("X-BT-UID:")) {
                    parts = UNESCAPE_COLON.split(line);
                    if (parts.length == 2) {
                        String[] subParts = UNESCAPE_COLON.split(parts[1]);
                        if (btUids == null) {
                            btUids = new ArrayList<String>(1);
                        }
                        btUids.add(subParts[subParts.length - 1]); // only keep actual UID
                    }
                    // Empty UID entry - ignore
                }

                line = reader.getLineEnforce();
            }
            return new VCard(
                    name,
                    formattedName,
                    phoneNumbers == null
                            ? null
                            : phoneNumbers.toArray(new String[phoneNumbers.size()]),
                    emailAddresses == null
                            ? null
                            : emailAddresses.toArray(new String[emailAddresses.size()]),
                    envLevel);
        }
    }
    ;

    @VisibleForTesting
    static class BMsgReader {
        InputStream mInStream;

        BMsgReader(InputStream is) {
            this.mInStream = is;
        }

        private byte[] getLineAsBytes() {
            int readByte;

            /* TODO: Actually the vCard spec. allows to break lines by using a newLine
             * followed by a white space character(space or tab). Not sure this is a good idea to
             * implement as the Bluetooth MAP spec. illustrates vCards using tab alignment,
             * hence actually showing an invalid vCard format...
             * If we read such a folded line, the folded part will be skipped in the parser
             * UPDATE: Check if we actually do unfold before parsing the input stream
             */

            ByteArrayOutputStream output = new ByteArrayOutputStream();
            try {
                while ((readByte = mInStream.read()) != -1) {
                    if (readByte == '\r') {
                        if ((readByte = mInStream.read()) != -1 && readByte == '\n') {
                            if (output.size() == 0) {
                                continue; /* Skip empty lines */
                            } else {
                                break;
                            }
                        } else {
                            output.write('\r');
                        }
                    } else if (readByte == '\n' && output.size() == 0) {
                        /* Empty line - skip */
                        continue;
                    }

                    output.write(readByte);
                }
            } catch (IOException e) {
                ContentProfileErrorReportUtils.report(
                        BluetoothProfile.MAP,
                        BluetoothProtoEnums.BLUETOOTH_MAP_BMESSAGE,
                        BluetoothStatsLog.BLUETOOTH_CONTENT_PROFILE_ERROR_REPORTED__TYPE__EXCEPTION,
                        0);
                Log.w(TAG, e);
                return null;
            }
            return output.toByteArray();
        }

        /**
         * Read a line of text from the BMessage.
         *
         * @return the next line of text, or null at end of file, or if UTF-8 is not supported.
         */
        public String getLine() {
            byte[] line = getLineAsBytes();
            if (line.length == 0) {
                return null;
            } else {
                return new String(line, StandardCharsets.UTF_8);
            }
        }

        /**
         * same as getLine(), but throws an exception, if we run out of lines. Use this function
         * when ever more lines are needed for the bMessage to be complete.
         *
         * @return the next line
         */
        public String getLineEnforce() {
            String line = getLine();
            if (line == null) {
                throw new IllegalArgumentException("Bmessage too short");
            }

            return line;
        }

        /**
         * Reads a line from the InputStream, and examines if the subString matches the line read.
         *
         * @param subString The string to match against the line.
         * @throws IllegalArgumentException If the expected substring is not found.
         */
        public void expect(String subString) throws IllegalArgumentException {
            String line = getLine();
            if (line == null || subString == null) {
                throw new IllegalArgumentException("Line or substring is null");
            } else if (!Ascii.toUpperCase(line).contains(Ascii.toUpperCase(subString))) {
                throw new IllegalArgumentException(
                        "Expected \"" + subString + "\" in: \"" + line + "\"");
            }
        }

        /**
         * Same as expect(String), but with two strings.
         *
         * @throws IllegalArgumentException If one of the strings are not found.
         */
        public void expect(String subString, String subString2) throws IllegalArgumentException {
            String line = getLine();
            if (!Ascii.toUpperCase(line).contains(Ascii.toUpperCase(subString))) {
                throw new IllegalArgumentException(
                        "Expected \"" + subString + "\" in: \"" + line + "\"");
            }
            if (!Ascii.toUpperCase(line).contains(Ascii.toUpperCase(subString2))) {
                throw new IllegalArgumentException(
                        "Expected \"" + subString + "\" in: \"" + line + "\"");
            }
        }

        /**
         * Read a part of the bMessage as raw data.
         *
         * @param length the number of bytes to read
         * @return the byte[] containing the number of bytes or null if an error occurs or EOF is
         *     reached before length bytes have been read.
         */
        public byte[] getDataBytes(int length) {
            byte[] data = new byte[length];
            try {
                int bytesRead;
                int offset = 0;
                while ((bytesRead = mInStream.read(data, offset, length - offset))
                        != (length - offset)) {
                    if (bytesRead == -1) {
                        return null;
                    }
                    offset += bytesRead;
                }
            } catch (IOException e) {
                ContentProfileErrorReportUtils.report(
                        BluetoothProfile.MAP,
                        BluetoothProtoEnums.BLUETOOTH_MAP_BMESSAGE,
                        BluetoothStatsLog.BLUETOOTH_CONTENT_PROFILE_ERROR_REPORTED__TYPE__EXCEPTION,
                        2);
                Log.w(TAG, e);
                return null;
            }
            return data;
        }
    }
    ;

    public BluetoothMapbMessage() {}

    public String getVersionString() {
        return mVersionString;
    }

    /**
     * Set the version string for VCARD
     *
     * @param version the actual number part of the version string i.e. 1.0
     */
    public void setVersionString(String version) {
        this.mVersionString = "VERSION:" + version;
    }

    public static BluetoothMapbMessage parse(InputStream bMsgStream, int appParamCharset)
            throws IllegalArgumentException {
        BMsgReader reader;
        BluetoothMapbMessage newBMsg = null;
        boolean status = false;
        boolean statusFound = false;
        TYPE type = null;
        String folder = null;

        reader = new BMsgReader(bMsgStream);
        reader.expect("BEGIN:BMSG");
        reader.expect("VERSION");

        String line = reader.getLineEnforce();
        // Parse the properties - which end with either a VCARD or a BENV
        while (!line.contains("BEGIN:VCARD") && !line.contains("BEGIN:BENV")) {
            if (line.contains("STATUS")) {
                String[] arg = COLON.split(line);
                if (arg != null && arg.length == 2) {
                    if (arg[1].trim().equals("READ")) {
                        status = true;
                    } else if (arg[1].trim().equals("UNREAD")) {
                        status = false;
                    } else {
                        throw new IllegalArgumentException("Wrong value in 'STATUS': " + arg[1]);
                    }
                } else {
                    throw new IllegalArgumentException("Missing value for 'STATUS': " + line);
                }
            }
            if (line.contains("EXTENDEDDATA")) {
                String[] arg = COLON.split(line);
                if (arg != null && arg.length == 2) {
                    String value = arg[1].trim();
                    // FIXME what should we do with this
                    Log.i(TAG, "We got extended data with: " + value);
                }
            }
            if (line.contains("TYPE")) {
                String[] arg = COLON.split(line);
                if (arg != null && arg.length == 2) {
                    String value = arg[1].trim();
                    /* Will throw IllegalArgumentException if value is wrong */
                    type = TYPE.valueOf(value);
                    if (appParamCharset == BluetoothMapAppParams.CHARSET_NATIVE
                            && type != TYPE.SMS_CDMA
                            && type != TYPE.SMS_GSM) {
                        throw new IllegalArgumentException(
                                "Native appParamsCharset " + "only supported for SMS");
                    }
                    switch (type) {
                        case SMS_CDMA:
                        case SMS_GSM:
                            newBMsg = new BluetoothMapbMessageSms();
                            break;
                        case MMS:
                            newBMsg = new BluetoothMapbMessageMime();
                            break;
                        case EMAIL:
                            newBMsg = new BluetoothMapbMessageEmail();
                            break;
                        case IM:
                            newBMsg = new BluetoothMapbMessageMime();
                            break;
                        default:
                            break;
                    }
                } else {
                    throw new IllegalArgumentException("Missing value for 'TYPE':" + line);
                }
            }
            if (line.contains("FOLDER")) {
                String[] arg = COLON.split(line);
                if (arg != null && arg.length == 2) {
                    folder = arg[1].trim();
                }
                // This can be empty for push message - hence ignore if there is no value
            }
            line = reader.getLineEnforce();
        }
        if (newBMsg == null) {
            throw new IllegalArgumentException(
                    "Missing bMessage TYPE: " + "- unable to parse body-content");
        }
        newBMsg.setType(type);
        newBMsg.mAppParamCharset = appParamCharset;
        if (folder != null) {
            newBMsg.setCompleteFolder(folder);
        }
        if (statusFound) {
            newBMsg.setStatus(status);
        }

        // Now check for originator VCARDs
        while (line.contains("BEGIN:VCARD")) {
            Log.d(TAG, "Decoding vCard");
            newBMsg.addOriginator(VCard.parseVcard(reader, 0));
            line = reader.getLineEnforce();
        }
        if (line.contains("BEGIN:BENV")) {
            newBMsg.parseEnvelope(reader, 0);
        } else {
            throw new IllegalArgumentException("Bmessage has no BEGIN:BENV - line:" + line);
        }

        /* TODO: Do we need to validate the END:* tags? They are only needed if someone puts
         *        additional info below the END:MSG - in which case we don't handle it.
         *        We need to parse the message based on the length field, to ensure MAP 1.0
         *        compatibility, since this spec. do not suggest to escape the end-tag if it
         *        occurs inside the message text.
         */

        try {
            bMsgStream.close();
        } catch (IOException e) {
            ContentProfileErrorReportUtils.report(
                    BluetoothProfile.MAP,
                    BluetoothProtoEnums.BLUETOOTH_MAP_BMESSAGE,
                    BluetoothStatsLog.BLUETOOTH_CONTENT_PROFILE_ERROR_REPORTED__TYPE__EXCEPTION,
                    7);
            /* Ignore if we cannot close the stream. */
        }

        return newBMsg;
    }

    private void parseEnvelope(BMsgReader reader, int level) {
        String line;
        line = reader.getLineEnforce();
        Log.d(TAG, "Decoding envelope level " + level);

        while (line.contains("BEGIN:VCARD")) {
            Log.d(TAG, "Decoding recipient vCard level " + level);
            if (mRecipient == null) {
                mRecipient = new ArrayList<VCard>(1);
            }
            mRecipient.add(VCard.parseVcard(reader, level));
            line = reader.getLineEnforce();
        }
        if (line.contains("BEGIN:BENV")) {
            Log.d(TAG, "Decoding nested envelope");
            parseEnvelope(reader, ++level); // Nested BENV
        }
        if (line.contains("BEGIN:BBODY")) {
            Log.d(TAG, "Decoding bbody");
            parseBody(reader);
        }
    }

    private void parseBody(BMsgReader reader) {
        String line;
        line = reader.getLineEnforce();
        parseMsgInit();
        while (!line.contains("END:")) {
            if (line.contains("PARTID:")) {
                String[] arg = COLON.split(line);
                if (arg != null && arg.length == 2) {
                    try {
                        Long unusedId = Long.parseLong(arg[1].trim());
                    } catch (NumberFormatException e) {
                        ContentProfileErrorReportUtils.report(
                                BluetoothProfile.MAP,
                                BluetoothProtoEnums.BLUETOOTH_MAP_BMESSAGE,
                                BluetoothStatsLog
                                        .BLUETOOTH_CONTENT_PROFILE_ERROR_REPORTED__TYPE__EXCEPTION,
                                8);
                        throw new IllegalArgumentException("Wrong value in 'PARTID': " + arg[1]);
                    }
                } else {
                    throw new IllegalArgumentException("Missing value for 'PARTID': " + line);
                }
            } else if (line.contains("ENCODING:")) {
                String[] arg = COLON.split(line);
                if (arg != null && arg.length == 2) {
                    mEncoding = arg[1].trim();
                    // If needed validation will be done when the value is used
                } else {
                    throw new IllegalArgumentException("Missing value for 'ENCODING': " + line);
                }
            } else if (line.contains("CHARSET:")) {
                String[] arg = COLON.split(line);
                if (arg != null && arg.length == 2) {
                    mCharset = arg[1].trim();
                    // If needed validation will be done when the value is used
                } else {
                    throw new IllegalArgumentException("Missing value for 'CHARSET': " + line);
                }
            } else if (line.contains("LANGUAGE:")) {
                String[] arg = COLON.split(line);
                if (arg != null && arg.length == 2) {
                    String unusedLanguage = arg[1].trim();
                    // If needed validation will be done when the value is used
                } else {
                    throw new IllegalArgumentException("Missing value for 'LANGUAGE': " + line);
                }
            } else if (line.contains("LENGTH:")) {
                String[] arg = COLON.split(line);
                if (arg != null && arg.length == 2) {
                    try {
                        mBMsgLength = Integer.parseInt(arg[1].trim());
                    } catch (NumberFormatException e) {
                        throw new IllegalArgumentException("Wrong value in 'LENGTH': " + arg[1]);
                    }
                } else {
                    throw new IllegalArgumentException("Missing value for 'LENGTH': " + line);
                }
            } else if (line.contains("BEGIN:MSG")) {
                Log.v(TAG, "bMsgLength: " + mBMsgLength);
                if (mBMsgLength == INVALID_VALUE) {
                    throw new IllegalArgumentException(
                            "Missing value for 'LENGTH'. "
                                    + "Unable to read remaining part of the message");
                }

                /* For SMS: Encoding of MSG is always UTF-8 compliant, regardless of any properties,
                since PDUs are encodes as hex-strings */
                /* PTS has a bug regarding the message length, and sets it 2 bytes too short, hence
                 * using the length field to determine the amount of data to read, might not be the
                 * best solution.
                 * Errata ESR06 section 5.8.12 introduced escaping of END:MSG in the actual message
                 * content, it is now safe to use the END:MSG tag as terminator, and simply ignore
                 * the length field.*/

                // Read until we receive END:MSG as some carkits send bad message lengths
                StringBuilder data = new StringBuilder();
                String messageLine = "";
                while (!messageLine.equals("END:MSG")) {
                    data.append(messageLine);
                    messageLine = reader.getLineEnforce();
                }

                // The MAP spec says that all END:MSG strings in the body
                // of the message must be escaped upon encoding and the
                // escape removed upon decoding
                parseMsgPart(data.toString().replaceAll("([/]*)/END\\:MSG", "$1END:MSG").trim());
            }
            line = reader.getLineEnforce();
        }
    }

    /** Parse the 'message' part of <bmessage-body-content>" */
    public abstract void parseMsgPart(String msgPart);

    /**
     * Set initial values before parsing - will be called is a message body is found during parsing.
     */
    public abstract void parseMsgInit();

    public abstract byte[] encode();

    public void setStatus(boolean read) {
        if (read) {
            this.mStatus = "READ";
        } else {
            this.mStatus = "UNREAD";
        }
    }

    public void setType(TYPE type) {
        this.mType = type;
    }

    /**
     * @return the type
     */
    public TYPE getType() {
        return mType;
    }

    public void setCompleteFolder(String folder) {
        this.mFolder = folder;
    }

    public void setFolder(String folder) {
        this.mFolder = "telecom/msg/" + folder;
    }

    public String getFolder() {
        return mFolder;
    }

    public void setEncoding(String encoding) {
        this.mEncoding = encoding;
    }

    public List<VCard> getOriginators() {
        return mOriginator;
    }

    public void addOriginator(VCard originator) {
        if (this.mOriginator == null) {
            this.mOriginator = new ArrayList<VCard>();
        }
        this.mOriginator.add(originator);
    }

    /**
     * Add a version 3.0 vCard with a formatted name
     *
     * @param name e.g. Bonde;Casper
     * @param formattedName e.g. "Casper Bonde"
     */
    public void addOriginator(
            String name,
            String formattedName,
            String[] phoneNumbers,
            String[] emailAddresses,
            String[] btUids,
            String[] btUcis) {
        if (mOriginator == null) {
            mOriginator = new ArrayList<VCard>();
        }
        mOriginator.add(
                new VCard(name, formattedName, phoneNumbers, emailAddresses, btUids, btUcis));
    }

    public void addOriginator(String[] btUcis, String[] btUids) {
        if (mOriginator == null) {
            mOriginator = new ArrayList<VCard>();
        }
        mOriginator.add(new VCard(null, null, null, null, btUids, btUcis));
    }

    /**
     * Add a version 2.1 vCard with only a name.
     *
     * @param name e.g. Bonde;Casper
     */
    public void addOriginator(String name, String[] phoneNumbers, String[] emailAddresses) {
        if (mOriginator == null) {
            mOriginator = new ArrayList<VCard>();
        }
        mOriginator.add(new VCard(name, phoneNumbers, emailAddresses));
    }

    public List<VCard> getRecipients() {
        return mRecipient;
    }

    public void setRecipient(VCard recipient) {
        if (this.mRecipient == null) {
            this.mRecipient = new ArrayList<VCard>();
        }
        this.mRecipient.add(recipient);
    }

    public void addRecipient(String[] btUcis, String[] btUids) {
        if (mRecipient == null) {
            mRecipient = new ArrayList<VCard>();
        }
        mRecipient.add(new VCard(null, null, null, null, btUids, btUcis));
    }

    public void addRecipient(
            String name,
            String formattedName,
            String[] phoneNumbers,
            String[] emailAddresses,
            String[] btUids,
            String[] btUcis) {
        if (mRecipient == null) {
            mRecipient = new ArrayList<VCard>();
        }
        mRecipient.add(
                new VCard(name, formattedName, phoneNumbers, emailAddresses, btUids, btUcis));
    }

    public void addRecipient(String name, String[] phoneNumbers, String[] emailAddresses) {
        if (mRecipient == null) {
            mRecipient = new ArrayList<VCard>();
        }
        mRecipient.add(new VCard(name, phoneNumbers, emailAddresses));
    }

    /**
     * Convert a byte[] of data to a hex string representation, converting each nibble to the
     * corresponding hex char. NOTE: There is not need to escape instances of "\r\nEND:MSG" in the
     * binary data represented as a string as only the characters [0-9] and [a-f] is used.
     *
     * @param pduData the byte-array of data.
     * @param scAddressData the byte-array of the encoded sc-Address.
     * @return the resulting string.
     */
    protected String encodeBinary(byte[] pduData, byte[] scAddressData) {
        StringBuilder out = new StringBuilder((pduData.length + scAddressData.length) * 2);
        for (int i = 0; i < scAddressData.length; i++) {
            out.append(Integer.toString((scAddressData[i] >> 4) & 0x0f, 16)); // MS-nibble first
            out.append(Integer.toString(scAddressData[i] & 0x0f, 16));
        }
        for (int i = 0; i < pduData.length; i++) {
            out.append(Integer.toString((pduData[i] >> 4) & 0x0f, 16)); // MS-nibble first
            out.append(Integer.toString(pduData[i] & 0x0f, 16));
            /*out.append(Integer.toHexString(data[i]));*/
            /* This is the same as above, but does not
             * include the needed 0's
             * e.g. it converts the value 3 to "3"
             * and not "03" */
        }
        return out.toString();
    }

    /**
     * Decodes a binary hex-string encoded UTF-8 string to the represented binary data set.
     *
     * @param data The string representation of the data - must have an even number of characters.
     * @return the byte[] represented in the data.
     */
    protected byte[] decodeBinary(String data) {
        byte[] out = new byte[data.length() / 2];
        String value;
        Log.d(TAG, "Decoding binary data: START:" + data + ":END");
        for (int i = 0, j = 0, n = out.length; i < n; i++, j += 2) {
            value = data.substring(j, j + 2);
            out[i] = (byte) (Integer.valueOf(value, 16) & 0xff);
        }

        // The following is a large enough debug operation such that we want to guard it with an
        // isLoggable check
        if (Log.isLoggable(TAG, Log.DEBUG)) {
            StringBuilder sb = new StringBuilder(out.length);
            for (int i = 0, n = out.length; i < n; i++) {
                sb.append(String.format("%02X", out[i] & 0xff));
            }
            Log.d(TAG, "Decoded binary data: START:" + sb.toString() + ":END");
        }

        return out;
    }

    public byte[] encodeGeneric(List<byte[]> bodyFragments) {
        StringBuilder sb = new StringBuilder(256);
        byte[] msgStart, msgEnd;
        sb.append("BEGIN:BMSG").append("\r\n");

        sb.append(mVersionString).append("\r\n");
        sb.append("STATUS:").append(mStatus).append("\r\n");
        sb.append("TYPE:").append(mType.name()).append("\r\n");
        if (mFolder.length() > 512) {
            sb.append("FOLDER:")
                    .append(mFolder.substring(mFolder.length() - 512, mFolder.length()))
                    .append("\r\n");
        } else {
            sb.append("FOLDER:").append(mFolder).append("\r\n");
        }
        if (!mVersionString.contains("1.0")) {
            sb.append("EXTENDEDDATA:").append("\r\n");
        }
        if (mOriginator != null) {
            for (VCard element : mOriginator) {
                element.encode(sb);
            }
        }
        /* If we need the three levels of env. at some point - we do have a level in the
         *  vCards that could be used to determine the levels of the envelope.
         */

        sb.append("BEGIN:BENV").append("\r\n");
        if (mRecipient != null) {
            for (VCard element : mRecipient) {
                Log.v(TAG, "encodeGeneric: recipient email" + element.getFirstEmail());
                element.encode(sb);
            }
        }
        sb.append("BEGIN:BBODY").append("\r\n");
        if (mEncoding != null && !mEncoding.isEmpty()) {
            sb.append("ENCODING:").append(mEncoding).append("\r\n");
        }
        if (mCharset != null && !mCharset.isEmpty()) {
            sb.append("CHARSET:").append(mCharset).append("\r\n");
        }

        int length = 0;
        /* 22 is the length of the 'BEGIN:MSG' and 'END:MSG' + 3*CRLF */
        for (byte[] fragment : bodyFragments) {
            length += fragment.length + 22;
        }
        sb.append("LENGTH:").append(length).append("\r\n");

        // Extract the initial part of the bMessage string
        msgStart = sb.toString().getBytes(StandardCharsets.UTF_8);

        sb = new StringBuilder(31);
        sb.append("END:BBODY").append("\r\n");
        sb.append("END:BENV").append("\r\n");
        sb.append("END:BMSG").append("\r\n");

        msgEnd = sb.toString().getBytes(StandardCharsets.UTF_8);

        try {

            ByteArrayOutputStream stream =
                    new ByteArrayOutputStream(msgStart.length + msgEnd.length + length);
            stream.write(msgStart);

            for (byte[] fragment : bodyFragments) {
                stream.write("BEGIN:MSG\r\n".getBytes(StandardCharsets.UTF_8));
                stream.write(fragment);
                stream.write("\r\nEND:MSG\r\n".getBytes(StandardCharsets.UTF_8));
            }
            stream.write(msgEnd);

            Log.v(TAG, stream.toString(StandardCharsets.UTF_8));
            return stream.toByteArray();
        } catch (IOException e) {
            ContentProfileErrorReportUtils.report(
                    BluetoothProfile.MAP,
                    BluetoothProtoEnums.BLUETOOTH_MAP_BMESSAGE,
                    BluetoothStatsLog.BLUETOOTH_CONTENT_PROFILE_ERROR_REPORTED__TYPE__EXCEPTION,
                    9);
            Log.w(TAG, e);
            return null;
        }
    }
}
