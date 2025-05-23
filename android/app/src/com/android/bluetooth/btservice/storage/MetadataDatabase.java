/*
 * Copyright 2019 The Android Open Source Project
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

package com.android.bluetooth.btservice.storage;

import android.content.Context;
import android.database.Cursor;
import android.database.SQLException;

import androidx.room.Database;
import androidx.room.Room;
import androidx.room.RoomDatabase;
import androidx.room.migration.Migration;
import androidx.sqlite.db.SupportSQLiteDatabase;

import com.android.internal.annotations.VisibleForTesting;

import java.util.List;

/** MetadataDatabase is a Room database stores Bluetooth persistence data */
@Database(
        entities = {Metadata.class},
        version = 121)
public abstract class MetadataDatabase extends RoomDatabase {
    /** The metadata database file name */
    public static final String DATABASE_NAME = "bluetooth_db";

    static int sCurrentConnectionNumber = 0;

    protected abstract MetadataDao mMetadataDao();

    /**
     * Create a {@link MetadataDatabase} database with migrations
     *
     * @param context the Context to create database
     * @return the created {@link MetadataDatabase}
     */
    public static MetadataDatabase createDatabase(Context context) {
        return Room.databaseBuilder(context, MetadataDatabase.class, DATABASE_NAME)
                .addMigrations(MIGRATION_100_101)
                .addMigrations(MIGRATION_101_102)
                .addMigrations(MIGRATION_102_103)
                .addMigrations(MIGRATION_103_104)
                .addMigrations(MIGRATION_104_105)
                .addMigrations(MIGRATION_105_106)
                .addMigrations(MIGRATION_106_107)
                .addMigrations(MIGRATION_107_108)
                .addMigrations(MIGRATION_108_109)
                .addMigrations(MIGRATION_109_110)
                .addMigrations(MIGRATION_110_111)
                .addMigrations(MIGRATION_111_112)
                .addMigrations(MIGRATION_112_113)
                .addMigrations(MIGRATION_113_114)
                .addMigrations(MIGRATION_114_115)
                .addMigrations(MIGRATION_115_116)
                .addMigrations(MIGRATION_116_117)
                .addMigrations(MIGRATION_117_118)
                .addMigrations(MIGRATION_118_119)
                .addMigrations(MIGRATION_119_120)
                .addMigrations(MIGRATION_120_121)
                .allowMainThreadQueries()
                .build();
    }

    /**
     * Create a {@link MetadataDatabase} database without migration, database would be reset if any
     * load failure happens
     *
     * @param context the Context to create database
     * @return the created {@link MetadataDatabase}
     */
    public static MetadataDatabase createDatabaseWithoutMigration(Context context) {
        return Room.databaseBuilder(context, MetadataDatabase.class, DATABASE_NAME)
                .fallbackToDestructiveMigration()
                .allowMainThreadQueries()
                .build();
    }

    /**
     * Insert a {@link Metadata} to metadata table
     *
     * @param metadata the data wish to put into storage
     */
    public void insert(Metadata... metadata) {
        mMetadataDao().insert(metadata);
    }

    /**
     * Load all data from metadata table as a {@link List} of {@link Metadata}
     *
     * @return a {@link List} of {@link Metadata}
     */
    public List<Metadata> load() {
        return mMetadataDao().load();
    }

    /**
     * Delete one of the {@link Metadata} contained in the metadata table
     *
     * @param address the address of Metadata to delete
     */
    public void delete(String address) {
        mMetadataDao().delete(address);
    }

    /** Clear metadata table. */
    public void deleteAll() {
        mMetadataDao().deleteAll();
    }

    @VisibleForTesting
    static final Migration MIGRATION_100_101 =
            new Migration(100, 101) {
                @Override
                public void migrate(SupportSQLiteDatabase database) {
                    database.execSQL(
                            "ALTER TABLE metadata ADD COLUMN `pbap_client_priority` INTEGER");
                }
            };

    @VisibleForTesting
    static final Migration MIGRATION_101_102 =
            new Migration(101, 102) {
                @Override
                public void migrate(SupportSQLiteDatabase database) {
                    database.execSQL(
                            "CREATE TABLE IF NOT EXISTS `metadata_tmp` (`address` TEXT NOT NULL,"
                                + " `migrated` INTEGER NOT NULL, `a2dpSupportsOptionalCodecs`"
                                + " INTEGER NOT NULL, `a2dpOptionalCodecsEnabled` INTEGER NOT NULL,"
                                + " `a2dp_priority` INTEGER, `a2dp_sink_priority` INTEGER,"
                                + " `hfp_priority` INTEGER, `hfp_client_priority` INTEGER,"
                                + " `hid_host_priority` INTEGER, `pan_priority` INTEGER,"
                                + " `pbap_priority` INTEGER, `pbap_client_priority` INTEGER,"
                                + " `map_priority` INTEGER, `sap_priority` INTEGER,"
                                + " `hearing_aid_priority` INTEGER, `map_client_priority` INTEGER,"
                                + " `manufacturer_name` BLOB, `model_name` BLOB, `software_version`"
                                + " BLOB, `hardware_version` BLOB, `companion_app` BLOB,"
                                + " `main_icon` BLOB, `is_untethered_headset` BLOB,"
                                + " `untethered_left_icon` BLOB, `untethered_right_icon` BLOB,"
                                + " `untethered_case_icon` BLOB, `untethered_left_battery` BLOB,"
                                + " `untethered_right_battery` BLOB, `untethered_case_battery`"
                                + " BLOB, `untethered_left_charging` BLOB,"
                                + " `untethered_right_charging` BLOB, `untethered_case_charging`"
                                + " BLOB, `enhanced_settings_ui_uri` BLOB, PRIMARY"
                                + " KEY(`address`))");

                    database.execSQL(
                            "INSERT INTO metadata_tmp (address, migrated,"
                                + " a2dpSupportsOptionalCodecs, a2dpOptionalCodecsEnabled,"
                                + " a2dp_priority, a2dp_sink_priority, hfp_priority,"
                                + " hfp_client_priority, hid_host_priority, pan_priority,"
                                + " pbap_priority, pbap_client_priority, map_priority,"
                                + " sap_priority, hearing_aid_priority, map_client_priority,"
                                + " manufacturer_name, model_name, software_version,"
                                + " hardware_version, companion_app, main_icon,"
                                + " is_untethered_headset, untethered_left_icon,"
                                + " untethered_right_icon, untethered_case_icon,"
                                + " untethered_left_battery, untethered_right_battery,"
                                + " untethered_case_battery, untethered_left_charging,"
                                + " untethered_right_charging, untethered_case_charging,"
                                + " enhanced_settings_ui_uri) SELECT address, migrated,"
                                + " a2dpSupportsOptionalCodecs, a2dpOptionalCodecsEnabled,"
                                + " a2dp_priority, a2dp_sink_priority, hfp_priority,"
                                + " hfp_client_priority, hid_host_priority, pan_priority,"
                                + " pbap_priority, pbap_client_priority, map_priority,"
                                + " sap_priority, hearing_aid_priority, map_client_priority, CAST"
                                + " (manufacturer_name AS BLOB), CAST (model_name AS BLOB), CAST"
                                + " (software_version AS BLOB), CAST (hardware_version AS BLOB),"
                                + " CAST (companion_app AS BLOB), CAST (main_icon AS BLOB), CAST"
                                + " (is_unthethered_headset AS BLOB), CAST (unthethered_left_icon"
                                + " AS BLOB), CAST (unthethered_right_icon AS BLOB), CAST"
                                + " (unthethered_case_icon AS BLOB), CAST (unthethered_left_battery"
                                + " AS BLOB), CAST (unthethered_right_battery AS BLOB), CAST"
                                + " (unthethered_case_battery AS BLOB), CAST"
                                + " (unthethered_left_charging AS BLOB), CAST"
                                + " (unthethered_right_charging AS BLOB), CAST"
                                + " (unthethered_case_charging AS BLOB), CAST"
                                + " (enhanced_settings_ui_uri AS BLOB)FROM metadata");

                    database.execSQL("DROP TABLE `metadata`");
                    database.execSQL("ALTER TABLE `metadata_tmp` RENAME TO `metadata`");
                }
            };

    @VisibleForTesting
    static final Migration MIGRATION_102_103 =
            new Migration(102, 103) {
                @Override
                public void migrate(SupportSQLiteDatabase database) {
                    try {
                        database.execSQL(
                                "CREATE TABLE IF NOT EXISTS `metadata_tmp` (`address` TEXT NOT"
                                    + " NULL, `migrated` INTEGER NOT NULL,"
                                    + " `a2dpSupportsOptionalCodecs` INTEGER NOT NULL,"
                                    + " `a2dpOptionalCodecsEnabled` INTEGER NOT NULL,"
                                    + " `a2dp_connection_policy` INTEGER,"
                                    + " `a2dp_sink_connection_policy` INTEGER,"
                                    + " `hfp_connection_policy` INTEGER,"
                                    + " `hfp_client_connection_policy` INTEGER,"
                                    + " `hid_host_connection_policy` INTEGER,"
                                    + " `pan_connection_policy` INTEGER, `pbap_connection_policy`"
                                    + " INTEGER, `pbap_client_connection_policy` INTEGER,"
                                    + " `map_connection_policy` INTEGER, `sap_connection_policy`"
                                    + " INTEGER, `hearing_aid_connection_policy` INTEGER,"
                                    + " `map_client_connection_policy` INTEGER, `manufacturer_name`"
                                    + " BLOB, `model_name` BLOB, `software_version` BLOB,"
                                    + " `hardware_version` BLOB, `companion_app` BLOB, `main_icon`"
                                    + " BLOB, `is_untethered_headset` BLOB, `untethered_left_icon`"
                                    + " BLOB, `untethered_right_icon` BLOB, `untethered_case_icon`"
                                    + " BLOB, `untethered_left_battery` BLOB,"
                                    + " `untethered_right_battery` BLOB, `untethered_case_battery`"
                                    + " BLOB, `untethered_left_charging` BLOB,"
                                    + " `untethered_right_charging` BLOB,"
                                    + " `untethered_case_charging` BLOB, `enhanced_settings_ui_uri`"
                                    + " BLOB, PRIMARY KEY(`address`))");

                        database.execSQL(
                                "INSERT INTO metadata_tmp (address, migrated,"
                                    + " a2dpSupportsOptionalCodecs, a2dpOptionalCodecsEnabled,"
                                    + " a2dp_connection_policy, a2dp_sink_connection_policy,"
                                    + " hfp_connection_policy,hfp_client_connection_policy,"
                                    + " hid_host_connection_policy,pan_connection_policy,"
                                    + " pbap_connection_policy,pbap_client_connection_policy,"
                                    + " map_connection_policy, sap_connection_policy,"
                                    + " hearing_aid_connection_policy,"
                                    + " map_client_connection_policy, manufacturer_name,"
                                    + " model_name, software_version, hardware_version,"
                                    + " companion_app, main_icon, is_untethered_headset,"
                                    + " untethered_left_icon, untethered_right_icon,"
                                    + " untethered_case_icon, untethered_left_battery,"
                                    + " untethered_right_battery, untethered_case_battery,"
                                    + " untethered_left_charging, untethered_right_charging,"
                                    + " untethered_case_charging, enhanced_settings_ui_uri) SELECT"
                                    + " address, migrated, a2dpSupportsOptionalCodecs,"
                                    + " a2dpOptionalCodecsEnabled, a2dp_priority,"
                                    + " a2dp_sink_priority, hfp_priority, hfp_client_priority,"
                                    + " hid_host_priority, pan_priority, pbap_priority,"
                                    + " pbap_client_priority, map_priority, sap_priority,"
                                    + " hearing_aid_priority, map_client_priority, CAST"
                                    + " (manufacturer_name AS BLOB), CAST (model_name AS BLOB),"
                                    + " CAST (software_version AS BLOB), CAST (hardware_version AS"
                                    + " BLOB), CAST (companion_app AS BLOB), CAST (main_icon AS"
                                    + " BLOB), CAST (is_untethered_headset AS BLOB), CAST"
                                    + " (untethered_left_icon AS BLOB), CAST (untethered_right_icon"
                                    + " AS BLOB), CAST (untethered_case_icon AS BLOB), CAST"
                                    + " (untethered_left_battery AS BLOB), CAST"
                                    + " (untethered_right_battery AS BLOB), CAST"
                                    + " (untethered_case_battery AS BLOB), CAST"
                                    + " (untethered_left_charging AS BLOB), CAST"
                                    + " (untethered_right_charging AS BLOB), CAST"
                                    + " (untethered_case_charging AS BLOB), CAST"
                                    + " (enhanced_settings_ui_uri AS BLOB)FROM metadata");

                        database.execSQL(
                                "UPDATE metadata_tmp SET a2dp_connection_policy = 100 "
                                        + "WHERE a2dp_connection_policy = 1000");
                        database.execSQL(
                                "UPDATE metadata_tmp SET a2dp_sink_connection_policy = 100 "
                                        + "WHERE a2dp_sink_connection_policy = 1000");
                        database.execSQL(
                                "UPDATE metadata_tmp SET hfp_connection_policy = 100 "
                                        + "WHERE hfp_connection_policy = 1000");
                        database.execSQL(
                                "UPDATE metadata_tmp SET hfp_client_connection_policy = 100 "
                                        + "WHERE hfp_client_connection_policy = 1000");
                        database.execSQL(
                                "UPDATE metadata_tmp SET hid_host_connection_policy = 100 "
                                        + "WHERE hid_host_connection_policy = 1000");
                        database.execSQL(
                                "UPDATE metadata_tmp SET pan_connection_policy = 100 "
                                        + "WHERE pan_connection_policy = 1000");
                        database.execSQL(
                                "UPDATE metadata_tmp SET pbap_connection_policy = 100 "
                                        + "WHERE pbap_connection_policy = 1000");
                        database.execSQL(
                                "UPDATE metadata_tmp SET pbap_client_connection_policy = 100 "
                                        + "WHERE pbap_client_connection_policy = 1000");
                        database.execSQL(
                                "UPDATE metadata_tmp SET map_connection_policy = 100 "
                                        + "WHERE map_connection_policy = 1000");
                        database.execSQL(
                                "UPDATE metadata_tmp SET sap_connection_policy = 100 "
                                        + "WHERE sap_connection_policy = 1000");
                        database.execSQL(
                                "UPDATE metadata_tmp SET hearing_aid_connection_policy = 100 "
                                        + "WHERE hearing_aid_connection_policy = 1000");
                        database.execSQL(
                                "UPDATE metadata_tmp SET map_client_connection_policy = 100 "
                                        + "WHERE map_client_connection_policy = 1000");

                        database.execSQL("DROP TABLE `metadata`");
                        database.execSQL("ALTER TABLE `metadata_tmp` RENAME TO `metadata`");
                    } catch (SQLException ex) {
                        // Check if user has new schema, but is just missing the version update
                        Cursor cursor = database.query("SELECT * FROM metadata");
                        if (cursor == null
                                || cursor.getColumnIndex("a2dp_connection_policy") == -1) {
                            throw ex;
                        }
                    }
                }
            };

    @VisibleForTesting
    static final Migration MIGRATION_103_104 =
            new Migration(103, 104) {
                @Override
                public void migrate(SupportSQLiteDatabase database) {
                    try {
                        database.execSQL(
                                "ALTER TABLE metadata ADD COLUMN `last_active_time` "
                                        + "INTEGER NOT NULL DEFAULT -1");
                        database.execSQL(
                                "ALTER TABLE metadata ADD COLUMN `is_active_a2dp_device` "
                                        + "INTEGER NOT NULL DEFAULT 0");
                    } catch (SQLException ex) {
                        // Check if user has new schema, but is just missing the version update
                        Cursor cursor = database.query("SELECT * FROM metadata");
                        if (cursor == null || cursor.getColumnIndex("last_active_time") == -1) {
                            throw ex;
                        }
                    }
                }
            };

    @VisibleForTesting
    static final Migration MIGRATION_104_105 =
            new Migration(104, 105) {
                @Override
                public void migrate(SupportSQLiteDatabase database) {
                    try {
                        database.execSQL("ALTER TABLE metadata ADD COLUMN `device_type` BLOB");
                        database.execSQL("ALTER TABLE metadata ADD COLUMN `main_battery` BLOB");
                        database.execSQL("ALTER TABLE metadata ADD COLUMN `main_charging` BLOB");
                        database.execSQL(
                                "ALTER TABLE metadata ADD COLUMN "
                                        + "`main_low_battery_threshold` BLOB");
                        database.execSQL(
                                "ALTER TABLE metadata ADD COLUMN "
                                        + "`untethered_left_low_battery_threshold` BLOB");
                        database.execSQL(
                                "ALTER TABLE metadata ADD COLUMN "
                                        + "`untethered_right_low_battery_threshold` BLOB");
                        database.execSQL(
                                "ALTER TABLE metadata ADD COLUMN "
                                        + "`untethered_case_low_battery_threshold` BLOB");
                    } catch (SQLException ex) {
                        // Check if user has new schema, but is just missing the version update
                        Cursor cursor = database.query("SELECT * FROM metadata");
                        if (cursor == null || cursor.getColumnIndex("device_type") == -1) {
                            throw ex;
                        }
                    }
                }
            };

    @VisibleForTesting
    static final Migration MIGRATION_105_106 =
            new Migration(105, 106) {
                @Override
                public void migrate(SupportSQLiteDatabase database) {
                    try {
                        database.execSQL(
                                "ALTER TABLE metadata ADD COLUMN `le_audio_connection_policy` "
                                        + "INTEGER DEFAULT 100");
                    } catch (SQLException ex) {
                        // Check if user has new schema, but is just missing the version update
                        Cursor cursor = database.query("SELECT * FROM metadata");
                        if (cursor == null
                                || cursor.getColumnIndex("le_audio_connection_policy") == -1) {
                            throw ex;
                        }
                    }
                }
            };

    @VisibleForTesting
    static final Migration MIGRATION_106_107 =
            new Migration(106, 107) {
                @Override
                public void migrate(SupportSQLiteDatabase database) {
                    try {
                        database.execSQL(
                                "ALTER TABLE metadata ADD COLUMN "
                                        + "`volume_control_connection_policy` INTEGER DEFAULT 100");
                    } catch (SQLException ex) {
                        // Check if user has new schema, but is just missing the version update
                        Cursor cursor = database.query("SELECT * FROM metadata");
                        if (cursor == null
                                || cursor.getColumnIndex("volume_control_connection_policy")
                                        == -1) {
                            throw ex;
                        }
                    }
                }
            };

    @VisibleForTesting
    static final Migration MIGRATION_107_108 =
            new Migration(107, 108) {
                @Override
                public void migrate(SupportSQLiteDatabase database) {
                    try {
                        database.execSQL(
                                "ALTER TABLE metadata ADD COLUMN"
                                    + " `csip_set_coordinator_connection_policy` INTEGER DEFAULT"
                                    + " 100");
                    } catch (SQLException ex) {
                        // Check if user has new schema, but is just missing the version update
                        Cursor cursor = database.query("SELECT * FROM metadata");
                        if (cursor == null
                                || cursor.getColumnIndex("csip_set_coordinator_connection_policy")
                                        == -1) {
                            throw ex;
                        }
                    }
                }
            };

    @VisibleForTesting
    static final Migration MIGRATION_108_109 =
            new Migration(108, 109) {
                @Override
                public void migrate(SupportSQLiteDatabase database) {
                    try {
                        database.execSQL(
                                "ALTER TABLE metadata ADD COLUMN"
                                    + " `le_call_control_connection_policy` INTEGER DEFAULT 100");
                    } catch (SQLException ex) {
                        // Check if user has new schema, but is just missing the version update
                        Cursor cursor = database.query("SELECT * FROM metadata");
                        if (cursor == null
                                || cursor.getColumnIndex("le_call_control_connection_policy")
                                        == -1) {
                            throw ex;
                        }
                    }
                }
            };

    @VisibleForTesting
    static final Migration MIGRATION_109_110 =
            new Migration(109, 110) {
                @Override
                public void migrate(SupportSQLiteDatabase database) {
                    try {
                        database.execSQL(
                                "ALTER TABLE metadata ADD COLUMN `hap_client_connection_policy` "
                                        + "INTEGER DEFAULT 100");
                    } catch (SQLException ex) {
                        // Check if user has new schema, but is just missing the version update
                        Cursor cursor = database.query("SELECT * FROM metadata");
                        if (cursor == null
                                || cursor.getColumnIndex("hap_client_connection_policy") == -1) {
                            throw ex;
                        }
                    }
                }
            };

    @VisibleForTesting
    static final Migration MIGRATION_110_111 =
            new Migration(110, 111) {
                @Override
                public void migrate(SupportSQLiteDatabase database) {
                    try {
                        database.execSQL(
                                "ALTER TABLE metadata ADD COLUMN `bass_client_connection_policy` "
                                        + "INTEGER DEFAULT 100");
                    } catch (SQLException ex) {
                        // Check if user has new schema, but is just missing the version update
                        Cursor cursor = database.query("SELECT * FROM metadata");
                        if (cursor == null
                                || cursor.getColumnIndex("bass_client_connection_policy") == -1) {
                            throw ex;
                        }
                    }
                }
            };

    @VisibleForTesting
    static final Migration MIGRATION_111_112 =
            new Migration(111, 112) {
                @Override
                public void migrate(SupportSQLiteDatabase database) {
                    try {
                        database.execSQL(
                                "ALTER TABLE metadata ADD COLUMN `battery_connection_policy` "
                                        + "INTEGER DEFAULT 100");
                    } catch (SQLException ex) {
                        // Check if user has new schema, but is just missing the version update
                        Cursor cursor = database.query("SELECT * FROM metadata");
                        if (cursor == null
                                || cursor.getColumnIndex("battery_connection_policy") == -1) {
                            throw ex;
                        }
                    }
                }
            };

    @VisibleForTesting
    static final Migration MIGRATION_112_113 =
            new Migration(112, 113) {
                @Override
                public void migrate(SupportSQLiteDatabase database) {
                    try {
                        database.execSQL("ALTER TABLE metadata ADD COLUMN `spatial_audio` BLOB");
                        database.execSQL(
                                "ALTER TABLE metadata ADD COLUMN `fastpair_customized` BLOB");
                    } catch (SQLException ex) {
                        // Check if user has new schema, but is just missing the version update
                        Cursor cursor = database.query("SELECT * FROM metadata");
                        if (cursor == null || cursor.getColumnIndex("spatial_audio") == -1) {
                            throw ex;
                        }
                    }
                }
            };

    @VisibleForTesting
    static final Migration MIGRATION_113_114 =
            new Migration(113, 114) {
                @Override
                public void migrate(SupportSQLiteDatabase database) {
                    try {
                        database.execSQL("ALTER TABLE metadata ADD COLUMN `le_audio` BLOB");
                    } catch (SQLException ex) {
                        // Check if user has new schema, but is just missing the version update
                        Cursor cursor = database.query("SELECT * FROM metadata");
                        if (cursor == null || cursor.getColumnIndex("le_audio") == -1) {
                            throw ex;
                        }
                    }
                }
            };

    @VisibleForTesting
    static final Migration MIGRATION_114_115 =
            new Migration(114, 115) {
                @Override
                public void migrate(SupportSQLiteDatabase database) {
                    try {
                        database.execSQL(
                                "ALTER TABLE metadata ADD COLUMN `call_establish_audio_policy` "
                                        + "INTEGER DEFAULT 0");
                        database.execSQL(
                                "ALTER TABLE metadata ADD COLUMN `connecting_time_audio_policy` "
                                        + "INTEGER DEFAULT 0");
                        database.execSQL(
                                "ALTER TABLE metadata ADD COLUMN `in_band_ringtone_audio_policy` "
                                        + "INTEGER DEFAULT 0");
                    } catch (SQLException ex) {
                        // Check if user has new schema, but is just missing the version update
                        Cursor cursor = database.query("SELECT * FROM metadata");
                        if (cursor == null
                                || cursor.getColumnIndex("call_establish_audio_policy") == -1) {
                            throw ex;
                        }
                    }
                }
            };

    @VisibleForTesting
    static final Migration MIGRATION_115_116 =
            new Migration(115, 116) {
                @Override
                public void migrate(SupportSQLiteDatabase database) {
                    try {
                        database.execSQL(
                                "ALTER TABLE metadata ADD COLUMN `preferred_output_only_profile` "
                                        + "INTEGER NOT NULL DEFAULT 0");
                        database.execSQL(
                                "ALTER TABLE metadata ADD COLUMN `preferred_duplex_profile` "
                                        + "INTEGER NOT NULL DEFAULT 0");
                    } catch (SQLException ex) {
                        // Check if user has new schema, but is just missing the version update
                        Cursor cursor = database.query("SELECT * FROM metadata");
                        if (cursor == null
                                || cursor.getColumnIndex("preferred_output_only_profile") == -1
                                || cursor.getColumnIndex("preferred_duplex_profile") == -1) {
                            throw ex;
                        }
                    }
                }
            };

    @VisibleForTesting
    static final Migration MIGRATION_116_117 =
            new Migration(116, 117) {
                @Override
                public void migrate(SupportSQLiteDatabase database) {
                    try {
                        database.execSQL("ALTER TABLE metadata ADD COLUMN `gmcs_cccd` BLOB");
                        database.execSQL("ALTER TABLE metadata ADD COLUMN `gtbs_cccd` BLOB");
                    } catch (SQLException ex) {
                        // Check if user has new schema, but is just missing the version update
                        Cursor cursor = database.query("SELECT * FROM metadata");
                        if (cursor == null || cursor.getColumnIndex("gmcs_cccd") == -1) {
                            throw ex;
                        }
                    }
                }
            };

    @VisibleForTesting
    static final Migration MIGRATION_117_118 =
            new Migration(117, 118) {
                @Override
                public void migrate(SupportSQLiteDatabase database) {
                    try {
                        database.execSQL(
                                "ALTER TABLE metadata ADD COLUMN `isActiveHfpDevice` "
                                        + "INTEGER NOT NULL DEFAULT 0");
                    } catch (SQLException ex) {
                        // Check if user has new schema, but is just missing the version update
                        Cursor cursor = database.query("SELECT * FROM metadata");
                        if (cursor == null || cursor.getColumnIndex("isActiveHfpDevice") == -1) {
                            throw ex;
                        }
                    }
                }
            };

    @VisibleForTesting
    static final Migration MIGRATION_118_119 =
            new Migration(118, 119) {
                @Override
                public void migrate(SupportSQLiteDatabase database) {
                    try {
                        database.execSQL(
                                "ALTER TABLE metadata ADD COLUMN `exclusive_manager` BLOB");
                    } catch (SQLException ex) {
                        // Check if user has new schema, but is just missing the version update
                        Cursor cursor = database.query("SELECT * FROM metadata");
                        if (cursor == null || cursor.getColumnIndex("exclusive_manager") == -1) {
                            throw ex;
                        }
                    }
                }
            };

    @VisibleForTesting
    static final Migration MIGRATION_119_120 =
            new Migration(119, 120) {
                @Override
                public void migrate(SupportSQLiteDatabase database) {
                    try {
                        database.execSQL(
                                "ALTER TABLE metadata ADD COLUMN"
                                        + " `active_audio_device_policy` INTEGER NOT NULL"
                                        + " DEFAULT 0");
                    } catch (SQLException ex) {
                        // Check if user has new schema, but is just missing the version update
                        Cursor cursor = database.query("SELECT * FROM metadata");
                        if (cursor == null
                                || cursor.getColumnIndex("active_audio_device_policy") == -1) {
                            throw ex;
                        }
                    }
                }
            };

    @VisibleForTesting
    static final Migration MIGRATION_120_121 =
            new Migration(120, 121) {
                @Override
                public void migrate(SupportSQLiteDatabase database) {
                    try {
                        database.execSQL(
                                "ALTER TABLE metadata ADD COLUMN"
                                        + " `is_preferred_microphone_for_calls` INTEGER NOT NULL"
                                        + " DEFAULT 1");
                    } catch (SQLException ex) {
                        // Check if user has new schema, but is just missing the version update
                        Cursor cursor = database.query("SELECT * FROM metadata");
                        if (cursor == null
                                || cursor.getColumnIndex("is_preferred_microphone_for_calls")
                                        == -1) {
                            throw ex;
                        }
                    }
                }
            };
}
