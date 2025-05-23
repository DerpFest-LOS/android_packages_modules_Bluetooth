//! Anything related to the adapter API (IBluetooth).

use bt_topshim::btif::{
    BaseCallbacks, BaseCallbacksDispatcher, BluetoothInterface, BluetoothProperty, BtAclState,
    BtAddrType, BtBondState, BtConnectionDirection, BtConnectionState, BtDeviceType, BtDiscMode,
    BtDiscoveryState, BtHciErrorCode, BtPinCode, BtPropertyType, BtScanMode, BtSspVariant, BtState,
    BtStatus, BtThreadEvent, BtTransport, BtVendorProductInfo, DisplayAddress, DisplayUuid,
    RawAddress, ToggleableProfile, Uuid, INVALID_RSSI,
};
use bt_topshim::{
    controller, metrics,
    profiles::gatt::GattStatus,
    profiles::hfp::EscoCodingFormat,
    profiles::hid_host::{
        BthhConnectionState, BthhHidInfo, BthhProtocolMode, BthhReportType, BthhStatus,
        HHCallbacks, HHCallbacksDispatcher, HidHost,
    },
    profiles::sdp::{BtSdpRecord, Sdp, SdpCallbacks, SdpCallbacksDispatcher},
    profiles::ProfileConnectionState,
    topstack,
};

use bt_utils::array_utils;
use bt_utils::cod::{is_cod_hid_combo, is_cod_hid_keyboard};
use bt_utils::uhid::UHid;
use btif_macros::{btif_callback, btif_callbacks_dispatcher};

use log::{debug, error, warn};
use num_derive::{FromPrimitive, ToPrimitive};
use num_traits::cast::ToPrimitive;
use num_traits::pow;
use std::collections::{HashMap, HashSet};
use std::convert::TryInto;
use std::fs::{File, OpenOptions};
use std::hash::Hash;
use std::io::Write;
use std::os::fd::AsRawFd;
use std::process;
use std::sync::{Arc, Condvar, Mutex};
use std::time::Duration;
use std::time::Instant;
use tokio::sync::mpsc::Sender;
use tokio::task::JoinHandle;
use tokio::time;

use crate::bluetooth_admin::BluetoothAdminPolicyHelper;
use crate::bluetooth_gatt::{
    BluetoothGatt, GattActions, IBluetoothGatt, IScannerCallback, ScanResult,
};
use crate::bluetooth_media::{BluetoothMedia, MediaActions, LEA_UNKNOWN_GROUP_ID};
use crate::callbacks::Callbacks;
use crate::socket_manager::SocketActions;
use crate::uuid::{Profile, UuidHelper};
use crate::{make_message_dispatcher, APIMessage, BluetoothAPI, Message, RPCProxy, SuspendMode};

pub(crate) const FLOSS_VER: u16 = 0x0001;
const DEFAULT_DISCOVERY_TIMEOUT_MS: u64 = 12800;
const MIN_ADV_INSTANCES_FOR_MULTI_ADV: u8 = 5;

/// Devices that were last seen longer than this duration are considered stale
/// if they haven't already bonded or connected. Once this duration expires, the
/// clear event should be sent to clients.
const FOUND_DEVICE_FRESHNESS: Duration = Duration::from_secs(30);

/// This is the value returned from Bluetooth Interface calls.
// TODO(241930383): Add enum to topshim
const BTM_SUCCESS: i32 = 0;

const PID_DIR: &str = "/var/run/bluetooth";

const DUMPSYS_LOG: &str = "/tmp/dumpsys.log";

/// Represents various roles the adapter supports.
#[derive(Debug, FromPrimitive, ToPrimitive)]
#[repr(u32)]
pub enum BtAdapterRole {
    Central = 0,
    Peripheral,
    CentralPeripheral,
}
/// Defines the adapter API.
pub trait IBluetooth {
    /// Adds a callback from a client who wishes to observe adapter events.
    fn register_callback(&mut self, callback: Box<dyn IBluetoothCallback + Send>) -> u32;

    /// Removes registered callback.
    fn unregister_callback(&mut self, callback_id: u32) -> bool;

    /// Adds a callback from a client who wishes to observe connection events.
    fn register_connection_callback(
        &mut self,
        callback: Box<dyn IBluetoothConnectionCallback + Send>,
    ) -> u32;

    /// Removes registered callback.
    fn unregister_connection_callback(&mut self, callback_id: u32) -> bool;

    /// Inits the bluetooth interface. Should always be called before enable.
    fn init(&mut self, hci_index: i32) -> bool;

    /// Enables the adapter.
    ///
    /// Returns true if the request is accepted.
    fn enable(&mut self) -> bool;

    /// Disables the adapter.
    ///
    /// Returns true if the request is accepted.
    fn disable(&mut self) -> bool;

    /// Cleans up the bluetooth interface. Should always be called after disable.
    fn cleanup(&mut self);

    /// Returns the Bluetooth address of the local adapter.
    fn get_address(&self) -> RawAddress;

    /// Gets supported UUIDs by the local adapter.
    fn get_uuids(&self) -> Vec<Uuid>;

    /// Gets the local adapter name.
    fn get_name(&self) -> String;

    /// Sets the local adapter name.
    fn set_name(&self, name: String) -> bool;

    /// Gets the bluetooth class.
    fn get_bluetooth_class(&self) -> u32;

    /// Sets the bluetooth class.
    fn set_bluetooth_class(&self, cod: u32) -> bool;

    /// Returns whether the adapter is discoverable.
    fn get_discoverable(&self) -> bool;

    /// Returns the adapter discoverable timeout.
    fn get_discoverable_timeout(&self) -> u32;

    /// Sets discoverability. If discoverable, limits the duration with given value.
    fn set_discoverable(&mut self, mode: BtDiscMode, duration: u32) -> bool;

    /// Returns whether multi-advertisement is supported.
    /// A minimum number of 5 advertising instances is required for multi-advertisment support.
    fn is_multi_advertisement_supported(&self) -> bool;

    /// Returns whether LE extended advertising is supported.
    fn is_le_extended_advertising_supported(&self) -> bool;

    /// Starts BREDR Inquiry.
    fn start_discovery(&mut self) -> bool;

    /// Cancels BREDR Inquiry.
    fn cancel_discovery(&mut self) -> bool;

    /// Checks if discovery is started.
    fn is_discovering(&self) -> bool;

    /// Checks when discovery ends in milliseconds from now.
    fn get_discovery_end_millis(&self) -> u64;

    /// Initiates pairing to a remote device. Triggers connection if not already started.
    fn create_bond(&mut self, device: BluetoothDevice, transport: BtTransport) -> BtStatus;

    /// Cancels any pending bond attempt on given device.
    fn cancel_bond_process(&mut self, device: BluetoothDevice) -> bool;

    /// Removes pairing for given device.
    fn remove_bond(&mut self, device: BluetoothDevice) -> bool;

    /// Returns a list of known bonded devices.
    fn get_bonded_devices(&self) -> Vec<BluetoothDevice>;

    /// Gets the bond state of a single device.
    fn get_bond_state(&self, device: BluetoothDevice) -> BtBondState;

    /// Set pin on bonding device.
    fn set_pin(&self, device: BluetoothDevice, accept: bool, pin_code: Vec<u8>) -> bool;

    /// Set passkey on bonding device.
    fn set_passkey(&self, device: BluetoothDevice, accept: bool, passkey: Vec<u8>) -> bool;

    /// Confirm that a pairing should be completed on a bonding device.
    fn set_pairing_confirmation(&self, device: BluetoothDevice, accept: bool) -> bool;

    /// Gets the name of the remote device.
    fn get_remote_name(&self, device: BluetoothDevice) -> String;

    /// Gets the type of the remote device.
    fn get_remote_type(&self, device: BluetoothDevice) -> BtDeviceType;

    /// Gets the alias of the remote device.
    fn get_remote_alias(&self, device: BluetoothDevice) -> String;

    /// Sets the alias of the remote device.
    fn set_remote_alias(&mut self, device: BluetoothDevice, new_alias: String);

    /// Gets the class of the remote device.
    fn get_remote_class(&self, device: BluetoothDevice) -> u32;

    /// Gets the appearance of the remote device.
    fn get_remote_appearance(&self, device: BluetoothDevice) -> u16;

    /// Gets whether the remote device is connected.
    fn get_remote_connected(&self, device: BluetoothDevice) -> bool;

    /// Gets whether the remote device can wake the system.
    fn get_remote_wake_allowed(&self, device: BluetoothDevice) -> bool;

    /// Gets the vendor and product information of the remote device.
    fn get_remote_vendor_product_info(&self, device: BluetoothDevice) -> BtVendorProductInfo;

    /// Get the address type of the remote device.
    fn get_remote_address_type(&self, device: BluetoothDevice) -> BtAddrType;

    /// Get the RSSI of the remote device.
    fn get_remote_rssi(&self, device: BluetoothDevice) -> i8;

    /// Returns a list of connected devices.
    fn get_connected_devices(&self) -> Vec<BluetoothDevice>;

    /// Gets the connection state of a single device.
    fn get_connection_state(&self, device: BluetoothDevice) -> BtConnectionState;

    /// Gets the connection state of a specific profile.
    fn get_profile_connection_state(&self, profile: Uuid) -> ProfileConnectionState;

    /// Returns the cached UUIDs of a remote device.
    fn get_remote_uuids(&self, device: BluetoothDevice) -> Vec<Uuid>;

    /// Triggers SDP to get UUIDs of a remote device.
    fn fetch_remote_uuids(&self, device: BluetoothDevice) -> bool;

    /// Triggers SDP and searches for a specific UUID on a remote device.
    fn sdp_search(&self, device: BluetoothDevice, uuid: Uuid) -> bool;

    /// Creates a new SDP record.
    fn create_sdp_record(&mut self, sdp_record: BtSdpRecord) -> bool;

    /// Removes the SDP record associated with the provided handle.
    fn remove_sdp_record(&self, handle: i32) -> bool;

    /// Connect all profiles supported by device and enabled on adapter.
    fn connect_all_enabled_profiles(&mut self, device: BluetoothDevice) -> BtStatus;

    /// Disconnect all profiles supported by device and enabled on adapter.
    /// Note that it includes all custom profiles enabled by the users e.g. through SocketManager or
    /// BluetoothGatt interfaces; The device shall be disconnected on baseband eventually.
    fn disconnect_all_enabled_profiles(&mut self, device: BluetoothDevice) -> bool;

    /// Returns whether WBS is supported.
    fn is_wbs_supported(&self) -> bool;

    /// Returns whether SWB is supported.
    fn is_swb_supported(&self) -> bool;

    /// Returns a list of all the roles that are supported.
    fn get_supported_roles(&self) -> Vec<BtAdapterRole>;

    /// Returns whether the coding format is supported.
    fn is_coding_format_supported(&self, coding_format: EscoCodingFormat) -> bool;

    /// Returns whether LE Audio is supported.
    fn is_le_audio_supported(&self) -> bool;

    /// Returns whether the remote device is a dual mode audio sink device (supports both classic and
    /// LE Audio sink roles).
    fn is_dual_mode_audio_sink_device(&self, device: BluetoothDevice) -> bool;

    /// Gets diagnostic output.
    fn get_dumpsys(&self) -> String;
}

/// Adapter API for Bluetooth qualification and verification.
///
/// This interface is provided for testing and debugging.
/// Clients should not use this interface for production.
pub trait IBluetoothQALegacy {
    /// Returns whether the adapter is connectable.
    fn get_connectable(&self) -> bool;

    /// Sets connectability. Returns true on success, false otherwise.
    fn set_connectable(&mut self, mode: bool) -> bool;

    /// Returns the adapter's Bluetooth friendly name.
    fn get_alias(&self) -> String;

    /// Returns the adapter's Device ID information in modalias format
    /// used by the kernel and udev.
    fn get_modalias(&self) -> String;

    /// Gets HID report on the peer.
    fn get_hid_report(
        &mut self,
        addr: RawAddress,
        report_type: BthhReportType,
        report_id: u8,
    ) -> BtStatus;

    /// Sets HID report to the peer.
    fn set_hid_report(
        &mut self,
        addr: RawAddress,
        report_type: BthhReportType,
        report: String,
    ) -> BtStatus;

    /// Snd HID data report to the peer.
    fn send_hid_data(&mut self, addr: RawAddress, data: String) -> BtStatus;
}

/// Action events from lib.rs
pub enum AdapterActions {
    /// Check whether the current set of found devices are still fresh.
    DeviceFreshnessCheck,

    /// Connect to all supported profiles on target device.
    ConnectAllProfiles(BluetoothDevice),

    /// Connect to the specified profiles on target device.
    ConnectProfiles(Vec<Uuid>, BluetoothDevice),

    /// Scanner for BLE discovery is registered with given status and scanner id.
    BleDiscoveryScannerRegistered(Uuid, u8, GattStatus),

    /// Scanner for BLE discovery is reporting a result.
    BleDiscoveryScannerResult(ScanResult),

    /// Reset the discoverable mode to BtDiscMode::NonDiscoverable.
    ResetDiscoverable,

    /// Create bond to the device stored in |pending_create_bond|.
    CreateBond,
}

/// Serializable device used in various apis.
#[derive(Clone, Debug, Default, PartialEq, Eq, Hash)]
pub struct BluetoothDevice {
    pub address: RawAddress,
    pub name: String,
}

impl BluetoothDevice {
    pub(crate) fn new(address: RawAddress, name: String) -> Self {
        Self { address, name }
    }

    pub(crate) fn from_properties(in_properties: &Vec<BluetoothProperty>) -> Self {
        let mut address = RawAddress::default();
        let mut name = String::from("");

        for prop in in_properties {
            match &prop {
                BluetoothProperty::BdAddr(bdaddr) => {
                    address = *bdaddr;
                }
                BluetoothProperty::BdName(bdname) => {
                    name = bdname.clone();
                }
                _ => {}
            }
        }

        Self { address, name }
    }
}

/// Internal data structure that keeps a map of cached properties for a remote device.
struct BluetoothDeviceContext {
    /// Transport type reported by ACL connection (if completed).
    pub acl_reported_transport: BtTransport,

    pub bredr_acl_state: BtAclState,
    pub ble_acl_state: BtAclState,
    pub bond_state: BtBondState,
    pub info: BluetoothDevice,
    pub last_seen: Instant,
    pub properties: HashMap<BtPropertyType, BluetoothProperty>,
    pub is_hh_connected: bool,

    /// If user wants to connect to all profiles, when new profiles are discovered we will also try
    /// to connect them.
    pub connect_to_new_profiles: bool,
}

impl BluetoothDeviceContext {
    pub(crate) fn new(
        bond_state: BtBondState,
        bredr_acl_state: BtAclState,
        ble_acl_state: BtAclState,
        info: BluetoothDevice,
        last_seen: Instant,
        properties: Vec<BluetoothProperty>,
    ) -> BluetoothDeviceContext {
        let mut device = BluetoothDeviceContext {
            acl_reported_transport: BtTransport::Auto,
            bredr_acl_state,
            ble_acl_state,
            bond_state,
            info,
            last_seen,
            properties: HashMap::new(),
            is_hh_connected: false,
            connect_to_new_profiles: false,
        };
        device.update_properties(&properties);
        device
    }

    pub(crate) fn update_properties(&mut self, in_properties: &Vec<BluetoothProperty>) {
        for prop in in_properties {
            // Handle merging of certain properties.
            match &prop {
                BluetoothProperty::BdAddr(bdaddr) => {
                    self.info.address = *bdaddr;
                    self.properties.insert(BtPropertyType::BdAddr, prop.clone());
                }
                BluetoothProperty::BdName(bdname) => {
                    if !bdname.is_empty() {
                        self.info.name = bdname.clone();
                        self.properties.insert(BtPropertyType::BdName, prop.clone());
                    }
                }
                BluetoothProperty::Uuids(new_uuids) => {
                    // Merge the new and the old (if exist) UUIDs.
                    self.properties
                        .entry(BtPropertyType::Uuids)
                        .and_modify(|old_prop| {
                            if let BluetoothProperty::Uuids(old_uuids) = old_prop {
                                for uuid in new_uuids {
                                    if !old_uuids.contains(uuid) {
                                        old_uuids.push(*uuid);
                                    }
                                }
                            }
                        })
                        .or_insert(prop.clone());
                }
                _ => {
                    self.properties.insert(prop.get_type(), prop.clone());
                }
            }
        }
    }

    /// Mark this device as seen.
    pub(crate) fn seen(&mut self) {
        self.last_seen = Instant::now();
    }

    fn get_default_transport(&self) -> BtTransport {
        self.properties.get(&BtPropertyType::TypeOfDevice).map_or(BtTransport::Auto, |prop| {
            match prop {
                BluetoothProperty::TypeOfDevice(t) => match *t {
                    BtDeviceType::Bredr => BtTransport::Bredr,
                    BtDeviceType::Ble => BtTransport::Le,
                    _ => BtTransport::Auto,
                },
                _ => BtTransport::Auto,
            }
        })
    }

    /// Check if it is connected in at least one transport.
    fn is_connected(&self) -> bool {
        self.bredr_acl_state == BtAclState::Connected || self.ble_acl_state == BtAclState::Connected
    }

    /// Set ACL state given transport. Return true if state changed.
    fn set_transport_state(&mut self, transport: &BtTransport, state: &BtAclState) -> bool {
        match (transport, self.get_default_transport()) {
            (t, d)
                if *t == BtTransport::Bredr
                    || (*t == BtTransport::Auto && d == BtTransport::Bredr) =>
            {
                if self.bredr_acl_state == *state {
                    return false;
                }
                self.bredr_acl_state = state.clone();
            }
            (t, d)
                if *t == BtTransport::Le || (*t == BtTransport::Auto && d == BtTransport::Le) =>
            {
                if self.ble_acl_state == *state {
                    return false;
                }
                self.ble_acl_state = state.clone();
            }
            // both link transport and the default transport are Auto.
            _ => {
                warn!("Unable to decide the transport! Set current connection states bredr({:?}), ble({:?}) to {:?}", self.bredr_acl_state, self.ble_acl_state, *state);
                if self.bredr_acl_state == *state && self.ble_acl_state == *state {
                    return false;
                }
                // There is no way for us to know which transport the link is referring to in this case.
                self.ble_acl_state = state.clone();
                self.bredr_acl_state = state.clone();
                return true;
            }
        };
        true
    }
}

/// Structure to track all the signals for SIGTERM.
pub struct SigData {
    pub enabled: Mutex<bool>,
    pub enabled_notify: Condvar,

    pub thread_attached: Mutex<bool>,
    pub thread_notify: Condvar,
}

/// The interface for adapter callbacks registered through `IBluetooth::register_callback`.
pub trait IBluetoothCallback: RPCProxy {
    /// When any adapter property changes.
    fn on_adapter_property_changed(&mut self, prop: BtPropertyType);

    /// When any device properties change.
    fn on_device_properties_changed(
        &mut self,
        remote_device: BluetoothDevice,
        props: Vec<BtPropertyType>,
    );

    /// When any of the adapter local address is changed.
    fn on_address_changed(&mut self, addr: RawAddress);

    /// When the adapter name is changed.
    fn on_name_changed(&mut self, name: String);

    /// When the adapter's discoverable mode is changed.
    fn on_discoverable_changed(&mut self, discoverable: bool);

    /// When a device is found via discovery.
    fn on_device_found(&mut self, remote_device: BluetoothDevice);

    /// When a device is cleared from discovered devices cache.
    fn on_device_cleared(&mut self, remote_device: BluetoothDevice);

    /// When the discovery state is changed.
    fn on_discovering_changed(&mut self, discovering: bool);

    /// When there is a pairing/bonding process and requires agent to display the event to UI.
    fn on_ssp_request(
        &mut self,
        remote_device: BluetoothDevice,
        cod: u32,
        variant: BtSspVariant,
        passkey: u32,
    );

    /// When there is a pin request to display the event to client.
    fn on_pin_request(&mut self, remote_device: BluetoothDevice, cod: u32, min_16_digit: bool);

    /// When there is a auto-gen pin to display the event to client.
    fn on_pin_display(&mut self, remote_device: BluetoothDevice, pincode: String);

    /// When a bonding attempt has completed.
    fn on_bond_state_changed(&mut self, status: u32, device_address: RawAddress, state: u32);

    /// When an SDP search has completed.
    fn on_sdp_search_complete(
        &mut self,
        remote_device: BluetoothDevice,
        searched_uuid: Uuid,
        sdp_records: Vec<BtSdpRecord>,
    );

    /// When an SDP record has been successfully created.
    fn on_sdp_record_created(&mut self, record: BtSdpRecord, handle: i32);
}

pub trait IBluetoothConnectionCallback: RPCProxy {
    /// Notification sent when a remote device completes HCI connection.
    fn on_device_connected(&mut self, remote_device: BluetoothDevice);

    /// Notification sent when a remote device completes HCI disconnection.
    fn on_device_disconnected(&mut self, remote_device: BluetoothDevice);

    /// Notification sent when a remote device fails to complete HCI connection.
    fn on_device_connection_failed(&mut self, remote_device: BluetoothDevice, status: BtStatus);
}

/// Implementation of the adapter API.
pub struct Bluetooth {
    intf: Arc<Mutex<BluetoothInterface>>,

    virt_index: i32,
    hci_index: i32,
    remote_devices: HashMap<RawAddress, BluetoothDeviceContext>,
    ble_scanner_id: Option<u8>,
    ble_scanner_uuid: Option<Uuid>,
    bluetooth_gatt: Option<Arc<Mutex<Box<BluetoothGatt>>>>,
    bluetooth_media: Option<Arc<Mutex<Box<BluetoothMedia>>>>,
    callbacks: Callbacks<dyn IBluetoothCallback + Send>,
    connection_callbacks: Callbacks<dyn IBluetoothConnectionCallback + Send>,
    discovering_started: Instant,
    hh: Option<HidHost>,
    is_connectable: bool,
    is_socket_listening: bool,
    discoverable_mode: BtDiscMode,
    discoverable_duration: u32,
    // This refers to the suspend mode of the functionality related to Classic scan mode,
    // i.e., page scan and inquiry scan; Also known as connectable and discoverable.
    scan_suspend_mode: SuspendMode,
    is_discovering: bool,
    is_discovering_before_suspend: bool,
    is_discovery_paused: bool,
    discovery_suspend_mode: SuspendMode,
    local_address: Option<RawAddress>,
    pending_discovery: bool,
    properties: HashMap<BtPropertyType, BluetoothProperty>,
    profiles_ready: bool,
    freshness_check: Option<JoinHandle<()>>,
    sdp: Option<Sdp>,
    state: BtState,
    disabling: bool,
    tx: Sender<Message>,
    api_tx: Sender<APIMessage>,
    // Internal API members
    discoverable_timeout: Option<JoinHandle<()>>,
    cancelling_devices: HashSet<RawAddress>,
    pending_create_bond: Option<(BluetoothDevice, BtTransport)>,
    active_pairing_address: Option<RawAddress>,
    le_supported_states: u64,
    le_local_supported_features: u64,

    /// Used to notify signal handler that we have turned off the stack.
    sig_notifier: Arc<SigData>,

    /// Virtual uhid device created to keep bluetooth as a wakeup source.
    uhid_wakeup_source: UHid,
}

impl Bluetooth {
    /// Constructs the IBluetooth implementation.
    pub fn new(
        virt_index: i32,
        hci_index: i32,
        tx: Sender<Message>,
        api_tx: Sender<APIMessage>,
        sig_notifier: Arc<SigData>,
        intf: Arc<Mutex<BluetoothInterface>>,
    ) -> Bluetooth {
        Bluetooth {
            virt_index,
            hci_index,
            remote_devices: HashMap::new(),
            callbacks: Callbacks::new(tx.clone(), Message::AdapterCallbackDisconnected),
            connection_callbacks: Callbacks::new(
                tx.clone(),
                Message::ConnectionCallbackDisconnected,
            ),
            hh: None,
            ble_scanner_id: None,
            ble_scanner_uuid: None,
            bluetooth_gatt: None,
            bluetooth_media: None,
            discovering_started: Instant::now(),
            intf,
            is_connectable: false,
            is_socket_listening: false,
            discoverable_mode: BtDiscMode::NonDiscoverable,
            discoverable_duration: 0,
            scan_suspend_mode: SuspendMode::Normal,
            is_discovering: false,
            is_discovering_before_suspend: false,
            is_discovery_paused: false,
            discovery_suspend_mode: SuspendMode::Normal,
            local_address: None,
            pending_discovery: false,
            properties: HashMap::new(),
            profiles_ready: false,
            freshness_check: None,
            sdp: None,
            state: BtState::Off,
            disabling: false,
            tx,
            api_tx,
            // Internal API members
            discoverable_timeout: None,
            cancelling_devices: HashSet::new(),
            pending_create_bond: None,
            active_pairing_address: None,
            le_supported_states: 0u64,
            le_local_supported_features: 0u64,
            sig_notifier,
            uhid_wakeup_source: UHid::new(),
        }
    }

    pub(crate) fn set_media(&mut self, bluetooth_media: Arc<Mutex<Box<BluetoothMedia>>>) {
        self.bluetooth_media = Some(bluetooth_media);
    }

    pub(crate) fn set_gatt_and_init_scanner(
        &mut self,
        bluetooth_gatt: Arc<Mutex<Box<BluetoothGatt>>>,
    ) {
        self.bluetooth_gatt = Some(bluetooth_gatt.clone());

        // Initialize the BLE scanner for discovery.
        let callback_id = bluetooth_gatt
            .lock()
            .unwrap()
            .register_scanner_callback(Box::new(BleDiscoveryCallbacks::new(self.tx.clone())));
        self.ble_scanner_uuid = Some(bluetooth_gatt.lock().unwrap().register_scanner(callback_id));
    }

    fn update_connectable_mode(&mut self) {
        // Don't bother if we are disabling. See b/361510982
        if self.disabling {
            return;
        }
        if self.get_scan_suspend_mode() != SuspendMode::Normal {
            return;
        }
        // Set connectable if
        // - there is bredr socket listening, or
        // - there is a classic device bonded and not connected
        self.set_connectable_internal(
            self.is_socket_listening
                || self.remote_devices.values().any(|ctx| {
                    ctx.bond_state == BtBondState::Bonded
                        && ctx.bredr_acl_state == BtAclState::Disconnected
                        && ctx
                            .properties
                            .get(&BtPropertyType::TypeOfDevice)
                            .and_then(|prop| match prop {
                                BluetoothProperty::TypeOfDevice(transport) => {
                                    Some(*transport != BtDeviceType::Ble)
                                }
                                _ => None,
                            })
                            .unwrap_or(false)
                }),
        );
    }

    pub(crate) fn set_socket_listening(&mut self, is_listening: bool) {
        if self.is_socket_listening == is_listening {
            return;
        }
        self.is_socket_listening = is_listening;
        self.update_connectable_mode();
    }

    pub(crate) fn get_hci_index(&self) -> u16 {
        self.hci_index as u16
    }

    pub(crate) fn handle_admin_policy_changed(
        &mut self,
        admin_policy_helper: BluetoothAdminPolicyHelper,
    ) {
        match (
            admin_policy_helper.is_profile_allowed(&Profile::Hid),
            self.hh.as_ref().unwrap().is_hidp_activated,
        ) {
            (true, false) => self.hh.as_mut().unwrap().activate_hidp(true),
            (false, true) => self.hh.as_mut().unwrap().activate_hidp(false),
            _ => {}
        }

        match (
            admin_policy_helper.is_profile_allowed(&Profile::Hogp),
            self.hh.as_ref().unwrap().is_hogp_activated,
        ) {
            (true, false) => self.hh.as_mut().unwrap().activate_hogp(true),
            (false, true) => self.hh.as_mut().unwrap().activate_hogp(false),
            _ => {}
        }

        if self.hh.as_mut().unwrap().configure_enabled_profiles() {
            self.hh.as_mut().unwrap().disable();
            let tx = self.tx.clone();

            tokio::spawn(async move {
                // Wait 100 milliseconds to prevent race condition caused by quick disable then
                // enable.
                // TODO: (b/272191117): don't enable until we're sure disable is done.
                tokio::time::sleep(Duration::from_millis(100)).await;
                let _ = tx.send(Message::HidHostEnable).await;
            });
        }
    }

    pub fn enable_hidhost(&mut self) {
        self.hh.as_mut().unwrap().enable();
    }

    pub fn init_profiles(&mut self) {
        self.sdp = Some(Sdp::new(&self.intf.lock().unwrap()));
        self.sdp.as_mut().unwrap().initialize(SdpCallbacksDispatcher {
            dispatch: make_message_dispatcher(self.tx.clone(), Message::Sdp),
        });

        self.hh = Some(HidHost::new(&self.intf.lock().unwrap()));
        self.hh.as_mut().unwrap().initialize(HHCallbacksDispatcher {
            dispatch: make_message_dispatcher(self.tx.clone(), Message::HidHost),
        });

        // Mark profiles as ready
        self.profiles_ready = true;
    }

    fn update_local_address(&mut self, addr: RawAddress) {
        self.local_address = Some(addr);

        self.callbacks.for_all_callbacks(|callback| {
            callback.on_address_changed(addr);
        });
    }

    pub(crate) fn adapter_callback_disconnected(&mut self, id: u32) {
        self.callbacks.remove_callback(id);
    }

    pub(crate) fn connection_callback_disconnected(&mut self, id: u32) {
        self.connection_callbacks.remove_callback(id);
    }

    fn get_remote_device_property(
        &self,
        device: &BluetoothDevice,
        property_type: &BtPropertyType,
    ) -> Option<BluetoothProperty> {
        self.remote_devices
            .get(&device.address)
            .and_then(|d| d.properties.get(property_type))
            .cloned()
    }

    fn set_remote_device_property(
        &mut self,
        device: &BluetoothDevice,
        property_type: BtPropertyType,
        property: BluetoothProperty,
    ) -> Result<(), ()> {
        let Some(remote_device) = self.remote_devices.get_mut(&device.address) else {
            return Err(());
        };

        // TODO: Determine why a callback isn't invoked to do this.
        remote_device.properties.insert(property_type, property.clone());
        self.intf.lock().unwrap().set_remote_device_property(&mut device.address.clone(), property);
        Ok(())
    }

    /// Returns whether the adapter is connectable.
    pub(crate) fn get_connectable_internal(&self) -> bool {
        self.discoverable_mode != BtDiscMode::NonDiscoverable || self.is_connectable
    }

    /// Sets the adapter's connectable mode for classic connections.
    pub(crate) fn set_connectable_internal(&mut self, mode: bool) -> bool {
        if self.get_scan_suspend_mode() != SuspendMode::Normal {
            // We will always trigger an update on resume so no need so store the mode change.
            return false;
        }
        if self.is_connectable == mode {
            return true;
        }
        if self.discoverable_mode != BtDiscMode::NonDiscoverable {
            // Discoverable always implies connectable. Don't affect the discoverable mode for now
            // and the connectable mode would be restored when discoverable becomes off.
            self.is_connectable = mode;
            return true;
        }
        self.intf.lock().unwrap().set_scan_mode(if mode {
            BtScanMode::Connectable
        } else {
            BtScanMode::None_
        });
        self.is_connectable = mode;
        true
    }

    /// Returns adapter's discoverable mode.
    pub(crate) fn get_discoverable_mode_internal(&self) -> BtDiscMode {
        self.discoverable_mode.clone()
    }

    /// Set the suspend mode for scan mode (connectable/discoverable mode).
    pub(crate) fn set_scan_suspend_mode(&mut self, suspend_mode: SuspendMode) {
        if suspend_mode != self.scan_suspend_mode {
            self.scan_suspend_mode = suspend_mode;
        }
    }

    /// Gets current suspend mode for scan mode (connectable/discoverable mode).
    pub(crate) fn get_scan_suspend_mode(&self) -> SuspendMode {
        self.scan_suspend_mode.clone()
    }

    /// Enters the suspend mode for scan mode (connectable/discoverable mode).
    pub(crate) fn scan_mode_enter_suspend(&mut self) -> BtStatus {
        if self.get_scan_suspend_mode() != SuspendMode::Normal {
            return BtStatus::Busy;
        }
        self.set_scan_suspend_mode(SuspendMode::Suspending);

        self.intf.lock().unwrap().set_scan_mode(BtScanMode::None_);

        self.set_scan_suspend_mode(SuspendMode::Suspended);

        BtStatus::Success
    }

    /// Exits the suspend mode for scan mode (connectable/discoverable mode).
    pub(crate) fn scan_mode_exit_suspend(&mut self) -> BtStatus {
        if self.get_scan_suspend_mode() != SuspendMode::Suspended {
            return BtStatus::Busy;
        }
        self.set_scan_suspend_mode(SuspendMode::Resuming);

        let mode = match self.discoverable_mode {
            BtDiscMode::LimitedDiscoverable => BtScanMode::ConnectableLimitedDiscoverable,
            BtDiscMode::GeneralDiscoverable => BtScanMode::ConnectableDiscoverable,
            BtDiscMode::NonDiscoverable => match self.is_connectable {
                true => BtScanMode::Connectable,
                false => BtScanMode::None_,
            },
        };
        self.intf.lock().unwrap().set_scan_mode(mode);

        self.set_scan_suspend_mode(SuspendMode::Normal);

        // Update is only available after SuspendMode::Normal
        self.update_connectable_mode();

        BtStatus::Success
    }

    /// Returns adapter's alias.
    pub(crate) fn get_alias_internal(&self) -> String {
        let name = self.get_name();
        if !name.is_empty() {
            return name;
        }

        // If the adapter name is empty, generate one based on local BDADDR
        // so that test programs can have a friendly name for the adapter.
        match self.local_address {
            None => "floss_0000".to_string(),
            Some(addr) => format!("floss_{:02X}{:02X}", addr.address[4], addr.address[5]),
        }
    }

    // TODO(b/328675014): Add BtAddrType and BtTransport parameters
    pub(crate) fn get_hid_report_internal(
        &mut self,
        mut addr: RawAddress,
        report_type: BthhReportType,
        report_id: u8,
    ) -> BtStatus {
        self.hh.as_mut().unwrap().get_report(
            &mut addr,
            BtAddrType::Public,
            BtTransport::Auto,
            report_type,
            report_id,
            128,
        )
    }

    // TODO(b/328675014): Add BtAddrType and BtTransport parameters
    pub(crate) fn set_hid_report_internal(
        &mut self,
        mut addr: RawAddress,
        report_type: BthhReportType,
        report: String,
    ) -> BtStatus {
        let mut rb = report.clone().into_bytes();
        self.hh.as_mut().unwrap().set_report(
            &mut addr,
            BtAddrType::Public,
            BtTransport::Auto,
            report_type,
            rb.as_mut_slice(),
        )
    }

    // TODO(b/328675014): Add BtAddrType and BtTransport parameters
    pub(crate) fn send_hid_data_internal(
        &mut self,
        mut addr: RawAddress,
        data: String,
    ) -> BtStatus {
        let mut rb = data.clone().into_bytes();
        self.hh.as_mut().unwrap().send_data(
            &mut addr,
            BtAddrType::Public,
            BtTransport::Auto,
            rb.as_mut_slice(),
        )
    }

    // TODO(b/328675014): Add BtAddrType and BtTransport parameters
    pub(crate) fn send_hid_virtual_unplug_internal(&mut self, mut addr: RawAddress) -> BtStatus {
        self.hh.as_mut().unwrap().virtual_unplug(&mut addr, BtAddrType::Public, BtTransport::Auto)
    }

    /// Returns all bonded and connected devices.
    pub(crate) fn get_bonded_and_connected_devices(&mut self) -> Vec<BluetoothDevice> {
        self.remote_devices
            .values()
            .filter(|v| v.is_connected() && v.bond_state == BtBondState::Bonded)
            .map(|v| v.info.clone())
            .collect()
    }

    /// Returns all devices with UUIDs, while None means there's not yet an UUID property change.
    pub(crate) fn get_all_devices_and_uuids(&self) -> Vec<(BluetoothDevice, Option<Vec<Uuid>>)> {
        self.remote_devices
            .values()
            .map(|d| {
                let uuids = d.properties.get(&BtPropertyType::Uuids).and_then(|prop| match prop {
                    BluetoothProperty::Uuids(uuids) => Some(uuids.clone()),
                    _ => None,
                });
                (d.info.clone(), uuids)
            })
            .collect()
    }

    /// Gets the bond state of a single device with its address.
    pub fn get_bond_state_by_addr(&self, addr: &RawAddress) -> BtBondState {
        self.remote_devices.get(addr).map_or(BtBondState::NotBonded, |d| d.bond_state.clone())
    }

    /// Gets whether a single device is connected with its address.
    fn get_acl_state_by_addr(&self, addr: &RawAddress) -> bool {
        self.remote_devices.get(addr).map_or(false, |d| d.is_connected())
    }

    /// Check whether remote devices are still fresh. If they're outside the
    /// freshness window, send a notification to clear the device from clients.
    fn trigger_freshness_check(&mut self) {
        // A remote device is considered fresh if:
        // * It was last seen less than |FOUND_DEVICE_FRESHNESS| ago.
        // * It is bonded / bonding (i.e., not NotBonded)
        // * It is currently connected.
        fn is_fresh(d: &BluetoothDeviceContext, now: &Instant) -> bool {
            let fresh_at = d.last_seen + FOUND_DEVICE_FRESHNESS;
            now < &fresh_at || d.is_connected() || d.bond_state != BtBondState::NotBonded
        }

        let now = Instant::now();
        let stale_devices: Vec<BluetoothDevice> = self
            .remote_devices
            .values()
            .filter(|d| !is_fresh(d, &now))
            .map(|d| d.info.clone())
            .collect();

        // Retain only devices that are fresh.
        self.remote_devices.retain(|_, d| is_fresh(d, &now));

        for d in stale_devices {
            self.callbacks.for_all_callbacks(|callback| {
                callback.on_device_cleared(d.clone());
            });
        }
    }

    /// Makes an LE_RAND call to the Bluetooth interface.
    pub fn le_rand(&mut self) -> bool {
        self.intf.lock().unwrap().le_rand() == BTM_SUCCESS
    }

    fn send_metrics_remote_device_info(device: &BluetoothDeviceContext) {
        if device.bond_state != BtBondState::Bonded && !device.is_connected() {
            return;
        }

        let mut class_of_device = 0u32;
        let mut device_type = BtDeviceType::Unknown;
        let mut appearance = 0u16;
        let mut vpi =
            BtVendorProductInfo { vendor_id_src: 0, vendor_id: 0, product_id: 0, version: 0 };

        for prop in device.properties.values() {
            match prop {
                BluetoothProperty::TypeOfDevice(p) => device_type = p.clone(),
                BluetoothProperty::ClassOfDevice(p) => class_of_device = *p,
                BluetoothProperty::Appearance(p) => appearance = *p,
                BluetoothProperty::VendorProductInfo(p) => vpi = *p,
                _ => (),
            }
        }

        metrics::device_info_report(
            device.info.address,
            device_type,
            class_of_device,
            appearance,
            vpi.vendor_id,
            vpi.vendor_id_src,
            vpi.product_id,
            vpi.version,
        );
    }

    /// Handle adapter actions.
    pub(crate) fn handle_actions(&mut self, action: AdapterActions) {
        match action {
            AdapterActions::DeviceFreshnessCheck => {
                self.trigger_freshness_check();
            }

            AdapterActions::ConnectAllProfiles(device) => {
                self.connect_all_enabled_profiles(device);
            }

            AdapterActions::ConnectProfiles(uuids, device) => {
                self.connect_profiles_internal(&uuids, device);
            }

            AdapterActions::BleDiscoveryScannerRegistered(uuid, scanner_id, status) => {
                if let Some(app_uuid) = self.ble_scanner_uuid {
                    if app_uuid == uuid {
                        if status == GattStatus::Success {
                            self.ble_scanner_id = Some(scanner_id);
                        } else {
                            log::error!("BLE discovery scanner failed to register: {:?}", status);
                        }
                    }
                }
            }

            AdapterActions::BleDiscoveryScannerResult(result) => {
                // Generate a vector of properties from ScanResult.
                let properties = {
                    let mut props = vec![];
                    props.push(BluetoothProperty::BdName(result.name.clone()));
                    props.push(BluetoothProperty::BdAddr(result.address));
                    if !result.service_uuids.is_empty() {
                        props.push(BluetoothProperty::Uuids(result.service_uuids.clone()));
                    }
                    if !result.service_data.is_empty() {
                        props.push(BluetoothProperty::Uuids(
                            result
                                .service_data
                                .keys()
                                .map(|v| Uuid::from_string(v).unwrap())
                                .collect(),
                        ));
                    }
                    props.push(BluetoothProperty::RemoteRssi(result.rssi));
                    props.push(BluetoothProperty::RemoteAddrType((result.addr_type as u32).into()));
                    props
                };

                let device_info = BluetoothDevice::from_properties(&properties);
                self.check_new_property_and_potentially_connect_profiles(
                    result.address,
                    &properties,
                );

                self.remote_devices
                    .entry(device_info.address)
                    .and_modify(|d| {
                        d.update_properties(&properties);
                        d.seen();
                    })
                    .or_insert(BluetoothDeviceContext::new(
                        BtBondState::NotBonded,
                        BtAclState::Disconnected,
                        BtAclState::Disconnected,
                        device_info,
                        Instant::now(),
                        properties,
                    ));
            }

            AdapterActions::ResetDiscoverable => {
                self.set_discoverable(BtDiscMode::NonDiscoverable, 0);
            }

            AdapterActions::CreateBond => {
                if let Some((device, transport)) = self.pending_create_bond.take() {
                    let status = self.create_bond(device, transport);
                    if status != BtStatus::Success {
                        error!("Failed CreateBond status={:?}", status);
                    }
                }
            }
        }
    }

    /// Creates a file to notify btmanagerd the adapter is enabled.
    fn create_pid_file(&self) -> std::io::Result<()> {
        let file_name = format!("{}/bluetooth{}.pid", PID_DIR, self.virt_index);
        let mut f = File::create(file_name)?;
        f.write_all(process::id().to_string().as_bytes())?;
        Ok(())
    }

    /// Removes the file to notify btmanagerd the adapter is disabled.
    fn remove_pid_file(&self) -> std::io::Result<()> {
        let file_name = format!("{}/bluetooth{}.pid", PID_DIR, self.virt_index);
        std::fs::remove_file(file_name)?;
        Ok(())
    }

    /// Set the suspend mode.
    pub fn set_discovery_suspend_mode(&mut self, suspend_mode: SuspendMode) {
        if suspend_mode != self.discovery_suspend_mode {
            self.discovery_suspend_mode = suspend_mode;
        }
    }

    /// Gets current suspend mode.
    pub fn get_discovery_suspend_mode(&self) -> SuspendMode {
        self.discovery_suspend_mode.clone()
    }

    /// Enters the suspend mode for discovery.
    pub fn discovery_enter_suspend(&mut self) -> BtStatus {
        if self.get_discovery_suspend_mode() != SuspendMode::Normal {
            return BtStatus::Busy;
        }
        self.set_discovery_suspend_mode(SuspendMode::Suspending);

        if self.is_discovering {
            self.is_discovering_before_suspend = true;
            self.cancel_discovery();
        }
        self.set_discovery_suspend_mode(SuspendMode::Suspended);

        BtStatus::Success
    }

    /// Exits the suspend mode for discovery.
    pub fn discovery_exit_suspend(&mut self) -> BtStatus {
        if self.get_discovery_suspend_mode() != SuspendMode::Suspended {
            return BtStatus::Busy;
        }
        self.set_discovery_suspend_mode(SuspendMode::Resuming);

        if self.is_discovering_before_suspend {
            self.is_discovering_before_suspend = false;
            self.start_discovery();
        }
        self.set_discovery_suspend_mode(SuspendMode::Normal);

        BtStatus::Success
    }

    /// Temporarily stop the discovery process and mark it as paused so that clients cannot restart
    /// it.
    fn pause_discovery(&mut self) {
        self.cancel_discovery();
        self.is_discovery_paused = true;
    }

    /// Remove the paused flag to allow clients to begin discovery, and if there is already a
    /// pending request, start discovery.
    fn resume_discovery(&mut self) {
        self.is_discovery_paused = false;
        if self.pending_discovery {
            self.pending_discovery = false;
            self.start_discovery();
        }
    }

    /// Return if there are wake-allowed device in bonded status.
    fn get_wake_allowed_device_bonded(&self) -> bool {
        self.get_bonded_devices().into_iter().any(|d| self.get_remote_wake_allowed(d))
    }

    /// Powerd recognizes bluetooth activities as valid wakeup sources if powerd keeps bluetooth in
    /// the monitored path. This only happens if there is at least one valid wake-allowed BT device
    /// connected during the suspending process. If there is no BT devices connected at any time
    /// during the suspending process, the wakeup count will be lost, and system goes to dark
    /// resume instead of full resume.
    /// Bluetooth stack disconnects all physical bluetooth HID devices for suspend, so a virtual
    /// uhid device is necessary to keep bluetooth as a valid wakeup source.
    fn create_uhid_for_suspend_wakesource(&mut self) {
        if !self.uhid_wakeup_source.is_empty() {
            return;
        }
        match self.uhid_wakeup_source.create(
            "VIRTUAL_SUSPEND_UHID".to_string(),
            self.get_address(),
            RawAddress::empty(),
        ) {
            Err(e) => error!("Fail to create uhid {}", e),
            Ok(_) => (),
        }
    }

    /// Clear the UHID device.
    fn clear_uhid(&mut self) {
        self.uhid_wakeup_source.clear();
    }

    /// Checks whether pairing is busy.
    pub fn is_pairing_busy(&self) -> bool {
        self.intf.lock().unwrap().pairing_is_busy()
            || self.active_pairing_address.is_some()
            || self.pending_create_bond.is_some()
    }

    pub fn is_hh_connected(&self, device_address: &RawAddress) -> bool {
        self.remote_devices.get(&device_address).map_or(false, |context| context.is_hh_connected)
    }

    /// Checks whether the list of device properties contains some UUID we should connect now
    /// This function also connects those UUIDs.
    fn check_new_property_and_potentially_connect_profiles(
        &self,
        addr: RawAddress,
        properties: &Vec<BluetoothProperty>,
    ) {
        // Return early if no need to connect new profiles
        if !self.remote_devices.get(&addr).map_or(false, |d| d.connect_to_new_profiles) {
            return;
        }

        // Get the reported UUIDs, if any. Otherwise return early.
        let mut new_uuids: Vec<Uuid> = vec![];
        for prop in properties.iter() {
            if let BluetoothProperty::Uuids(value) = prop {
                new_uuids.extend(value);
            }
        }
        if new_uuids.is_empty() {
            return;
        }

        // Only connect if the UUID is not seen before and it's supported
        let device = BluetoothDevice::new(addr, "".to_string());
        let current_uuids = self.get_remote_uuids(device.clone());
        new_uuids.retain(|uuid| !current_uuids.contains(uuid));

        let profile_known_and_supported = new_uuids.iter().any(|uuid| {
            if let Some(profile) = UuidHelper::is_known_profile(uuid) {
                return UuidHelper::is_profile_supported(&profile);
            }
            return false;
        });
        if !profile_known_and_supported {
            return;
        }

        log::info!("[{}]: Connecting to newly discovered profiles", DisplayAddress(&addr));
        let tx = self.tx.clone();
        tokio::spawn(async move {
            let _ = tx
                .send(Message::AdapterActions(AdapterActions::ConnectProfiles(new_uuids, device)))
                .await;
        });
    }

    /// Connect these profiles of a peripheral device
    fn connect_profiles_internal(&mut self, uuids: &Vec<Uuid>, device: BluetoothDevice) {
        let addr = device.address;
        if !self.get_acl_state_by_addr(&addr) {
            // log ACL connection attempt if it's not already connected.
            metrics::acl_connect_attempt(addr, BtAclState::Connected);
            // Pause discovery before connecting, or the ACL connection request may conflict with
            // the ongoing inquiry.
            self.pause_discovery();
        }

        let mut has_supported_profile = false;
        let mut has_le_media_profile = false;
        let mut has_classic_media_profile = false;

        for uuid in uuids.iter() {
            match UuidHelper::is_known_profile(uuid) {
                Some(p) => {
                    if UuidHelper::is_profile_supported(&p) {
                        match p {
                            Profile::Hid | Profile::Hogp => {
                                has_supported_profile = true;
                                // TODO(b/328675014): Use BtAddrType
                                // and BtTransport from
                                // BluetoothDevice instead of default
                                let status = self.hh.as_ref().unwrap().connect(
                                    &mut addr.clone(),
                                    BtAddrType::Public,
                                    BtTransport::Auto,
                                );
                                metrics::profile_connection_state_changed(
                                    addr,
                                    p as u32,
                                    BtStatus::Success,
                                    BthhConnectionState::Connecting as u32,
                                );

                                if status != BtStatus::Success {
                                    metrics::profile_connection_state_changed(
                                        addr,
                                        p as u32,
                                        status,
                                        BthhConnectionState::Disconnected as u32,
                                    );
                                }
                            }

                            // TODO(b/317682584): implement policy to connect to LEA, VC, and CSIS
                            Profile::LeAudio | Profile::VolumeControl | Profile::CoordinatedSet
                                if !has_le_media_profile =>
                            {
                                has_le_media_profile = true;
                                let txl = self.tx.clone();
                                topstack::get_runtime().spawn(async move {
                                    let _ = txl
                                        .send(Message::Media(
                                            MediaActions::ConnectLeaGroupByMemberAddress(addr),
                                        ))
                                        .await;
                                });
                            }

                            Profile::A2dpSink | Profile::A2dpSource | Profile::Hfp
                                if !has_classic_media_profile =>
                            {
                                has_supported_profile = true;
                                has_classic_media_profile = true;
                                let txl = self.tx.clone();
                                topstack::get_runtime().spawn(async move {
                                    let _ =
                                        txl.send(Message::Media(MediaActions::Connect(addr))).await;
                                });
                            }

                            // We don't connect most profiles
                            _ => (),
                        }
                    }
                }
                _ => {}
            }
        }

        // If the device does not have a profile that we are interested in connecting to, resume
        // discovery now. Other cases will be handled in the ACL connection state or bond state
        // callbacks.
        if !has_supported_profile {
            self.resume_discovery();
        }
    }

    fn fire_device_connection_or_bonded_state_changed(&self, addr: RawAddress) {
        if let Some(device) = self.remote_devices.get(&addr) {
            let tx = self.tx.clone();
            let bredr_acl_state = device.bredr_acl_state.clone();
            let ble_acl_state = device.ble_acl_state.clone();
            let bond_state = device.bond_state.clone();
            let transport = match self.get_remote_type(device.info.clone()) {
                BtDeviceType::Bredr => BtTransport::Bredr,
                BtDeviceType::Ble => BtTransport::Le,
                _ => device.acl_reported_transport.clone(),
            };
            tokio::spawn(async move {
                let _ = tx
                    .send(Message::OnDeviceConnectionOrBondStateChanged(
                        addr,
                        bredr_acl_state,
                        ble_acl_state,
                        bond_state,
                        transport,
                    ))
                    .await;
            });
        }
    }
}

#[btif_callbacks_dispatcher(dispatch_base_callbacks, BaseCallbacks)]
#[allow(unused_variables)]
pub(crate) trait BtifBluetoothCallbacks {
    #[btif_callback(AdapterState)]
    fn adapter_state_changed(&mut self, state: BtState) {}

    #[btif_callback(AdapterProperties)]
    fn adapter_properties_changed(
        &mut self,
        status: BtStatus,
        num_properties: i32,
        properties: Vec<BluetoothProperty>,
    ) {
    }

    #[btif_callback(DeviceFound)]
    fn device_found(&mut self, n: i32, properties: Vec<BluetoothProperty>) {}

    #[btif_callback(DiscoveryState)]
    fn discovery_state(&mut self, state: BtDiscoveryState) {}

    #[btif_callback(SspRequest)]
    fn ssp_request(&mut self, remote_addr: RawAddress, variant: BtSspVariant, passkey: u32) {}

    #[btif_callback(BondState)]
    fn bond_state(
        &mut self,
        status: BtStatus,
        addr: RawAddress,
        bond_state: BtBondState,
        fail_reason: i32,
    ) {
    }

    #[btif_callback(RemoteDeviceProperties)]
    fn remote_device_properties_changed(
        &mut self,
        status: BtStatus,
        addr: RawAddress,
        num_properties: i32,
        properties: Vec<BluetoothProperty>,
    ) {
    }

    #[btif_callback(AclState)]
    fn acl_state(
        &mut self,
        status: BtStatus,
        addr: RawAddress,
        state: BtAclState,
        link_type: BtTransport,
        hci_reason: BtHciErrorCode,
        conn_direction: BtConnectionDirection,
        acl_handle: u16,
    ) {
    }

    #[btif_callback(LeRandCallback)]
    fn le_rand_cb(&mut self, random: u64) {}

    #[btif_callback(PinRequest)]
    fn pin_request(
        &mut self,
        remote_addr: RawAddress,
        remote_name: String,
        cod: u32,
        min_16_digit: bool,
    ) {
    }

    #[btif_callback(ThreadEvent)]
    fn thread_event(&mut self, event: BtThreadEvent) {}
}

#[btif_callbacks_dispatcher(dispatch_hid_host_callbacks, HHCallbacks)]
pub(crate) trait BtifHHCallbacks {
    #[btif_callback(ConnectionState)]
    fn connection_state(
        &mut self,
        address: RawAddress,
        address_type: BtAddrType,
        transport: BtTransport,
        state: BthhConnectionState,
    );

    #[btif_callback(HidInfo)]
    fn hid_info(
        &mut self,
        address: RawAddress,
        address_type: BtAddrType,
        transport: BtTransport,
        info: BthhHidInfo,
    );

    #[btif_callback(ProtocolMode)]
    fn protocol_mode(
        &mut self,
        address: RawAddress,
        address_type: BtAddrType,
        transport: BtTransport,
        status: BthhStatus,
        mode: BthhProtocolMode,
    );

    #[btif_callback(IdleTime)]
    fn idle_time(
        &mut self,
        address: RawAddress,
        address_type: BtAddrType,
        transport: BtTransport,
        status: BthhStatus,
        idle_rate: i32,
    );

    #[btif_callback(GetReport)]
    fn get_report(
        &mut self,
        address: RawAddress,
        address_type: BtAddrType,
        transport: BtTransport,
        status: BthhStatus,
        data: Vec<u8>,
        size: i32,
    );

    #[btif_callback(Handshake)]
    fn handshake(
        &mut self,
        address: RawAddress,
        address_type: BtAddrType,
        transport: BtTransport,
        status: BthhStatus,
    );
}

#[btif_callbacks_dispatcher(dispatch_sdp_callbacks, SdpCallbacks)]
pub(crate) trait BtifSdpCallbacks {
    #[btif_callback(SdpSearch)]
    fn sdp_search(
        &mut self,
        status: BtStatus,
        address: RawAddress,
        uuid: Uuid,
        count: i32,
        records: Vec<BtSdpRecord>,
    );
}

pub fn get_bt_dispatcher(tx: Sender<Message>) -> BaseCallbacksDispatcher {
    BaseCallbacksDispatcher { dispatch: make_message_dispatcher(tx, Message::Base) }
}

impl BtifBluetoothCallbacks for Bluetooth {
    fn adapter_state_changed(&mut self, state: BtState) {
        let prev_state = self.state.clone();
        self.state = state;
        metrics::adapter_state_changed(self.state.clone());

        // If it's the same state as before, no further action
        if self.state == prev_state {
            return;
        }

        match self.state {
            BtState::Off => {
                self.properties.clear();
                match self.remove_pid_file() {
                    Err(err) => warn!("remove_pid_file() error: {}", err),
                    _ => (),
                }

                self.clear_uhid();

                // Let the signal notifier know we are turned off.
                *self.sig_notifier.enabled.lock().unwrap() = false;
                self.sig_notifier.enabled_notify.notify_all();
            }

            BtState::On => {
                // Initialize core profiles
                self.init_profiles();

                // Trigger properties update
                self.intf.lock().unwrap().get_adapter_properties();

                // Also need to manually request some properties
                self.intf.lock().unwrap().get_adapter_property(BtPropertyType::ClassOfDevice);
                let mut controller = controller::Controller::new();
                self.le_supported_states = controller.get_ble_supported_states();
                self.le_local_supported_features = controller.get_ble_local_supported_features();

                // Update connectable mode so that disconnected bonded classic device can reconnect
                self.update_connectable_mode();

                // Spawn a freshness check job in the background.
                if let Some(h) = self.freshness_check.take() {
                    h.abort()
                }
                let txl = self.tx.clone();
                self.freshness_check = Some(tokio::spawn(async move {
                    loop {
                        time::sleep(FOUND_DEVICE_FRESHNESS).await;
                        let _ = txl
                            .send(Message::AdapterActions(AdapterActions::DeviceFreshnessCheck))
                            .await;
                    }
                }));

                if self.get_wake_allowed_device_bonded() {
                    self.create_uhid_for_suspend_wakesource();
                }
                // Notify the signal notifier that we are turned on.
                *self.sig_notifier.enabled.lock().unwrap() = true;
                self.sig_notifier.enabled_notify.notify_all();

                // Signal that the stack is up and running.
                match self.create_pid_file() {
                    Err(err) => warn!("create_pid_file() error: {}", err),
                    _ => (),
                }

                // Inform the rest of the stack we're ready.
                let txl = self.tx.clone();
                let api_txl = self.api_tx.clone();
                tokio::spawn(async move {
                    let _ = txl.send(Message::AdapterReady).await;
                });
                tokio::spawn(async move {
                    let _ = api_txl.send(APIMessage::IsReady(BluetoothAPI::Adapter)).await;
                });
            }
        }
    }

    #[allow(unused_variables)]
    fn adapter_properties_changed(
        &mut self,
        status: BtStatus,
        num_properties: i32,
        properties: Vec<BluetoothProperty>,
    ) {
        if status != BtStatus::Success {
            return;
        }

        // Update local property cache
        for prop in properties {
            self.properties.insert(prop.get_type(), prop.clone());

            match &prop {
                BluetoothProperty::BdAddr(bdaddr) => {
                    self.update_local_address(*bdaddr);
                }
                BluetoothProperty::AdapterBondedDevices(bondlist) => {
                    for addr in bondlist.iter() {
                        self.remote_devices
                            .entry(*addr)
                            .and_modify(|d| d.bond_state = BtBondState::Bonded)
                            .or_insert(BluetoothDeviceContext::new(
                                BtBondState::Bonded,
                                BtAclState::Disconnected,
                                BtAclState::Disconnected,
                                BluetoothDevice::new(*addr, "".to_string()),
                                Instant::now(),
                                vec![],
                            ));
                    }

                    // Update the connectable mode since bonded device list might be updated.
                    self.update_connectable_mode();
                }
                BluetoothProperty::BdName(bdname) => {
                    self.callbacks.for_all_callbacks(|callback| {
                        callback.on_name_changed(bdname.clone());
                    });
                }
                _ => {}
            }

            self.callbacks.for_all_callbacks(|callback| {
                callback.on_adapter_property_changed(prop.get_type());
            });
        }
    }

    fn device_found(&mut self, _n: i32, properties: Vec<BluetoothProperty>) {
        let device_info = BluetoothDevice::from_properties(&properties);
        self.check_new_property_and_potentially_connect_profiles(device_info.address, &properties);

        let device_info = self
            .remote_devices
            .entry(device_info.address)
            .and_modify(|d| {
                d.update_properties(&properties);
                d.seen();
            })
            .or_insert(BluetoothDeviceContext::new(
                BtBondState::NotBonded,
                BtAclState::Disconnected,
                BtAclState::Disconnected,
                device_info,
                Instant::now(),
                properties,
            ))
            .info
            .clone();

        self.callbacks.for_all_callbacks(|callback| {
            callback.on_device_found(device_info.clone());
        });
    }

    fn discovery_state(&mut self, state: BtDiscoveryState) {
        let is_discovering = &state == &BtDiscoveryState::Started;

        // No-op if we're updating the state to the same value again.
        if &is_discovering == &self.is_discovering {
            return;
        }

        // Cache discovering state
        self.is_discovering = &state == &BtDiscoveryState::Started;
        if self.is_discovering {
            self.discovering_started = Instant::now();
        }

        // Prevent sending out discovering changes or freshness checks when
        // suspending. Clients don't need to be notified of discovery pausing
        // during suspend. They will probably try to restore it and fail.
        let discovery_suspend_mode = self.get_discovery_suspend_mode();
        if discovery_suspend_mode != SuspendMode::Normal
            && discovery_suspend_mode != SuspendMode::Resuming
        {
            return;
        }

        self.callbacks.for_all_callbacks(|callback| {
            callback.on_discovering_changed(state == BtDiscoveryState::Started);
        });

        // Start or stop BLE scanning based on discovering state
        if let (Some(gatt), Some(scanner_id)) = (self.bluetooth_gatt.as_ref(), self.ble_scanner_id)
        {
            if is_discovering {
                gatt.lock().unwrap().start_active_scan(scanner_id);
            } else {
                gatt.lock().unwrap().stop_active_scan(scanner_id);
            }
        }

        if !self.is_discovering && self.pending_create_bond.is_some() {
            debug!("Invoking delayed CreateBond");
            let tx = self.tx.clone();
            tokio::spawn(async move {
                let _ = tx.send(Message::AdapterActions(AdapterActions::CreateBond)).await;
            });
        }
    }

    fn ssp_request(&mut self, remote_addr: RawAddress, variant: BtSspVariant, passkey: u32) {
        // Accept the Just-Works pairing that we initiated, reject otherwise.
        if variant == BtSspVariant::Consent {
            let initiated_by_us = Some(remote_addr) == self.active_pairing_address;
            self.set_pairing_confirmation(
                BluetoothDevice::new(remote_addr, "".to_string()),
                initiated_by_us,
            );
            return;
        }

        // Currently this supports many agent because we accept many callbacks.
        // TODO(b/274706838): We need a way to select the default agent.
        self.callbacks.for_all_callbacks(|callback| {
            // TODO(b/336960912): libbluetooth changed their API so that we no longer
            // get the Device name and CoD, which were included in our DBus API.
            // Now we simply put random values since we aren't ready to change our DBus API
            // and it works because our Clients are not using these anyway.
            callback.on_ssp_request(
                BluetoothDevice::new(remote_addr, "".to_string()),
                0,
                variant.clone(),
                passkey,
            );
        });
    }

    fn pin_request(
        &mut self,
        remote_addr: RawAddress,
        remote_name: String,
        cod: u32,
        min_16_digit: bool,
    ) {
        let device = BluetoothDevice::new(remote_addr, remote_name.clone());

        let digits = match min_16_digit {
            true => 16,
            false => 6,
        };

        if is_cod_hid_keyboard(cod) || is_cod_hid_combo(cod) {
            debug!("auto gen pin for device {} (cod={:#x})", DisplayAddress(&remote_addr), cod);
            // generate a random pin code to display.
            let pin = rand::random::<u64>() % pow(10, digits);
            let display_pin = format!("{:06}", pin);

            // Currently this supports many agent because we accept many callbacks.
            // TODO(b/274706838): We need a way to select the default agent.
            self.callbacks.for_all_callbacks(|callback| {
                callback.on_pin_display(device.clone(), display_pin.clone());
            });

            let pin_vec = display_pin.chars().map(|d| d.try_into().unwrap()).collect::<Vec<u8>>();

            self.set_pin(device, true, pin_vec);
        } else {
            debug!(
                "sending pin request for device {} (cod={:#x}) to clients",
                DisplayAddress(&remote_addr),
                cod
            );
            // Currently this supports many agent because we accept many callbacks.
            // TODO(b/274706838): We need a way to select the default agent.
            self.callbacks.for_all_callbacks(|callback| {
                callback.on_pin_request(device.clone(), cod, min_16_digit);
            });
        }
    }

    fn bond_state(
        &mut self,
        status: BtStatus,
        addr: RawAddress,
        bond_state: BtBondState,
        fail_reason: i32,
    ) {
        // Get the device type before the device is potentially deleted.
        let device_type = self.get_remote_type(BluetoothDevice::new(addr, "".to_string()));

        // Clear the pairing lock if this call corresponds to the
        // active pairing device.
        if bond_state != BtBondState::Bonding && self.active_pairing_address == Some(addr) {
            self.active_pairing_address = None;
        }

        if self.get_bond_state_by_addr(&addr) == bond_state {
            debug!("[{}]: Unchanged bond_state", DisplayAddress(&addr));
        } else {
            let entry =
                self.remote_devices.entry(addr).and_modify(|d| d.bond_state = bond_state.clone());
            match bond_state {
                BtBondState::NotBonded => {
                    if !self.get_wake_allowed_device_bonded() {
                        self.clear_uhid();
                    }
                    // Update the connectable mode since bonded list is changed.
                    self.update_connectable_mode();
                }
                BtBondState::Bonded => {
                    let device = entry.or_insert(BluetoothDeviceContext::new(
                        BtBondState::Bonded,
                        BtAclState::Disconnected,
                        BtAclState::Disconnected,
                        BluetoothDevice::new(addr, "".to_string()),
                        Instant::now(),
                        vec![],
                    ));
                    let device_info = device.info.clone();
                    // Since this is a newly bonded device, we also need to trigger SDP on it.
                    self.fetch_remote_uuids(device_info);
                    if self.get_wake_allowed_device_bonded() {
                        self.create_uhid_for_suspend_wakesource();
                    }
                    // Update the connectable mode since bonded list is changed.
                    self.update_connectable_mode();
                }
                BtBondState::Bonding => {}
            }
        }

        // Modification to |self.remote_devices| has done, ok to fire the change event.
        self.fire_device_connection_or_bonded_state_changed(addr);

        // Resume discovery once the bonding process is complete. Discovery was paused before the
        // bond request to avoid ACL connection from interfering with active inquiry.
        if bond_state == BtBondState::NotBonded || bond_state == BtBondState::Bonded {
            self.resume_discovery();
        }

        // Send bond state changed notifications
        self.callbacks.for_all_callbacks(|callback| {
            callback.on_bond_state_changed(
                status.to_u32().unwrap(),
                addr,
                bond_state.to_u32().unwrap(),
            );
        });

        // Don't emit the metrics event if we were cancelling the bond.
        // It is ok to not send the pairing complete event as the server should ignore the dangling
        // pairing attempt event.
        // This behavior aligns with BlueZ.
        if !self.cancelling_devices.remove(&addr) {
            metrics::bond_state_changed(addr, device_type, status, bond_state, fail_reason);
        }
    }

    fn remote_device_properties_changed(
        &mut self,
        _status: BtStatus,
        addr: RawAddress,
        _num_properties: i32,
        properties: Vec<BluetoothProperty>,
    ) {
        self.check_new_property_and_potentially_connect_profiles(addr, &properties);
        let device = self.remote_devices.entry(addr).or_insert(BluetoothDeviceContext::new(
            BtBondState::NotBonded,
            BtAclState::Disconnected,
            BtAclState::Disconnected,
            BluetoothDevice::new(addr, String::from("")),
            Instant::now(),
            vec![],
        ));

        device.update_properties(&properties);
        device.seen();

        Bluetooth::send_metrics_remote_device_info(device);

        let info = device.info.clone();

        self.callbacks.for_all_callbacks(|callback| {
            callback.on_device_properties_changed(
                info.clone(),
                properties.clone().into_iter().map(|x| x.get_type()).collect(),
            );
        });

        // Only care about device type property changed on bonded device.
        // If the property change happens during bonding, it will be updated after bonding complete anyway.
        if self.get_bond_state_by_addr(&addr) == BtBondState::Bonded
            && properties.iter().any(|prop| match prop {
                BluetoothProperty::TypeOfDevice(_) => true,
                _ => false,
            })
        {
            // Update the connectable mode since the device type is changed.
            self.update_connectable_mode();
        }
    }

    fn acl_state(
        &mut self,
        status: BtStatus,
        addr: RawAddress,
        state: BtAclState,
        link_type: BtTransport,
        hci_reason: BtHciErrorCode,
        conn_direction: BtConnectionDirection,
        _acl_handle: u16,
    ) {
        // If discovery was previously paused at connect_all_enabled_profiles to avoid an outgoing
        // ACL connection colliding with an ongoing inquiry, resume it.
        self.resume_discovery();

        if status != BtStatus::Success {
            warn!(
                "Connection to [{}] failed. Status: {:?}, Reason: {:?}",
                DisplayAddress(&addr),
                status,
                hci_reason
            );
            metrics::acl_connection_state_changed(
                addr,
                link_type,
                status,
                BtAclState::Disconnected,
                conn_direction,
                hci_reason,
            );
            self.connection_callbacks.for_all_callbacks(|callback| {
                callback.on_device_connection_failed(
                    BluetoothDevice::new(addr, String::from("")),
                    status,
                );
            });
            return;
        }

        let device = self.remote_devices.entry(addr).or_insert(BluetoothDeviceContext::new(
            BtBondState::NotBonded,
            BtAclState::Disconnected,
            BtAclState::Disconnected,
            BluetoothDevice::new(addr, String::from("")),
            Instant::now(),
            vec![],
        ));

        // Only notify if there's been a change in state
        if !device.set_transport_state(&link_type, &state) {
            return;
        }

        let info = device.info.clone();
        device.acl_reported_transport = link_type;

        metrics::acl_connection_state_changed(
            addr,
            link_type,
            BtStatus::Success,
            state.clone(),
            conn_direction,
            hci_reason,
        );

        match state {
            BtAclState::Connected => {
                Bluetooth::send_metrics_remote_device_info(device);
                self.connection_callbacks.for_all_callbacks(|callback| {
                    callback.on_device_connected(info.clone());
                });
            }
            BtAclState::Disconnected => {
                if !device.is_connected() {
                    self.connection_callbacks.for_all_callbacks(|callback| {
                        callback.on_device_disconnected(info.clone());
                    });
                    device.connect_to_new_profiles = false;
                }
            }
        };

        // Modification to |self.remote_devices| has done, ok to fire the change event.
        self.fire_device_connection_or_bonded_state_changed(addr);

        // If we are bonding, skip the update here as we will update it after bonding complete anyway.
        // This is necessary for RTK controllers, which will break RNR after |Write Scan Enable|
        // command. Although this is a bug of RTK controllers, but as we could avoid unwanted page
        // scan, it makes sense to extend it to all BT controllers here.
        if Some(addr) != self.active_pairing_address {
            // Update the connectable since the connected state could be changed.
            self.update_connectable_mode();
        }
    }

    fn thread_event(&mut self, event: BtThreadEvent) {
        match event {
            BtThreadEvent::Associate => {
                // Let the signal notifier know stack is initialized.
                *self.sig_notifier.thread_attached.lock().unwrap() = true;
                self.sig_notifier.thread_notify.notify_all();
            }
            BtThreadEvent::Disassociate => {
                // Let the signal notifier know stack is done.
                *self.sig_notifier.thread_attached.lock().unwrap() = false;
                self.sig_notifier.thread_notify.notify_all();
            }
        }
    }
}

struct BleDiscoveryCallbacks {
    tx: Sender<Message>,
}

impl BleDiscoveryCallbacks {
    fn new(tx: Sender<Message>) -> Self {
        Self { tx }
    }
}

// Handle BLE scanner results.
impl IScannerCallback for BleDiscoveryCallbacks {
    fn on_scanner_registered(&mut self, uuid: Uuid, scanner_id: u8, status: GattStatus) {
        let tx = self.tx.clone();
        tokio::spawn(async move {
            let _ = tx
                .send(Message::AdapterActions(AdapterActions::BleDiscoveryScannerRegistered(
                    uuid, scanner_id, status,
                )))
                .await;
        });
    }

    fn on_scan_result(&mut self, scan_result: ScanResult) {
        let tx = self.tx.clone();
        tokio::spawn(async move {
            let _ = tx
                .send(Message::AdapterActions(AdapterActions::BleDiscoveryScannerResult(
                    scan_result,
                )))
                .await;
        });
    }

    fn on_advertisement_found(&mut self, _scanner_id: u8, _scan_result: ScanResult) {}
    fn on_advertisement_lost(&mut self, _scanner_id: u8, _scan_result: ScanResult) {}
    fn on_suspend_mode_change(&mut self, _suspend_mode: SuspendMode) {}
}

impl RPCProxy for BleDiscoveryCallbacks {
    fn get_object_id(&self) -> String {
        "BLE Discovery Callback".to_string()
    }
}

// TODO: Add unit tests for this implementation
impl IBluetooth for Bluetooth {
    fn register_callback(&mut self, callback: Box<dyn IBluetoothCallback + Send>) -> u32 {
        self.callbacks.add_callback(callback)
    }

    fn unregister_callback(&mut self, callback_id: u32) -> bool {
        self.callbacks.remove_callback(callback_id)
    }

    fn register_connection_callback(
        &mut self,
        callback: Box<dyn IBluetoothConnectionCallback + Send>,
    ) -> u32 {
        self.connection_callbacks.add_callback(callback)
    }

    fn unregister_connection_callback(&mut self, callback_id: u32) -> bool {
        self.connection_callbacks.remove_callback(callback_id)
    }

    fn init(&mut self, hci_index: i32) -> bool {
        self.intf.lock().unwrap().initialize(get_bt_dispatcher(self.tx.clone()), hci_index)
    }

    fn enable(&mut self) -> bool {
        self.disabling = false;
        self.intf.lock().unwrap().enable() == 0
    }

    fn disable(&mut self) -> bool {
        self.disabling = true;
        if !self.set_discoverable(BtDiscMode::NonDiscoverable, 0) {
            warn!("set_discoverable failed on disabling");
        }
        if !self.set_connectable_internal(false) {
            warn!("set_connectable_internal failed on disabling");
        }
        self.intf.lock().unwrap().disable() == 0
    }

    fn cleanup(&mut self) {
        self.intf.lock().unwrap().cleanup();
    }

    fn get_address(&self) -> RawAddress {
        self.local_address.unwrap_or_default()
    }

    fn get_uuids(&self) -> Vec<Uuid> {
        match self.properties.get(&BtPropertyType::Uuids) {
            Some(prop) => match prop {
                BluetoothProperty::Uuids(uuids) => uuids.clone(),
                _ => vec![],
            },
            _ => vec![],
        }
    }

    fn get_name(&self) -> String {
        match self.properties.get(&BtPropertyType::BdName) {
            Some(prop) => match prop {
                BluetoothProperty::BdName(name) => name.clone(),
                _ => String::new(),
            },
            _ => String::new(),
        }
    }

    fn set_name(&self, name: String) -> bool {
        if self.get_name() == name {
            return true;
        }
        self.intf.lock().unwrap().set_adapter_property(BluetoothProperty::BdName(name)) == 0
    }

    fn get_bluetooth_class(&self) -> u32 {
        match self.properties.get(&BtPropertyType::ClassOfDevice) {
            Some(prop) => match prop {
                BluetoothProperty::ClassOfDevice(cod) => *cod,
                _ => 0,
            },
            _ => 0,
        }
    }

    fn set_bluetooth_class(&self, cod: u32) -> bool {
        self.intf.lock().unwrap().set_adapter_property(BluetoothProperty::ClassOfDevice(cod)) == 0
    }

    fn get_discoverable(&self) -> bool {
        self.get_discoverable_mode_internal() != BtDiscMode::NonDiscoverable
    }

    fn get_discoverable_timeout(&self) -> u32 {
        self.discoverable_duration
    }

    fn set_discoverable(&mut self, mode: BtDiscMode, duration: u32) -> bool {
        let intf = self.intf.lock().unwrap();

        // Checks if the duration is valid.
        if mode == BtDiscMode::LimitedDiscoverable && (duration > 60 || duration == 0) {
            warn!("Invalid duration for setting the device into limited discoverable mode. The valid duration is 1~60 seconds.");
            return false;
        }

        // Don't really set the mode when suspend. The mode would be instead restored on resume.
        // However, we still need to set the discoverable timeout so it would properly reset
        // |self.discoverable_mode| after resume.
        if self.get_scan_suspend_mode() == SuspendMode::Normal {
            let scan_mode = match mode {
                BtDiscMode::LimitedDiscoverable => BtScanMode::ConnectableLimitedDiscoverable,
                BtDiscMode::GeneralDiscoverable => BtScanMode::ConnectableDiscoverable,
                BtDiscMode::NonDiscoverable => match self.is_connectable {
                    true => BtScanMode::Connectable,
                    false => BtScanMode::None_,
                },
            };
            intf.set_scan_mode(scan_mode);
        }

        self.callbacks.for_all_callbacks(|callback| {
            callback.on_discoverable_changed(mode == BtDiscMode::GeneralDiscoverable);
        });
        self.discoverable_mode = mode.clone();
        self.discoverable_duration = duration;

        // The old timer should be overwritten regardless of what the new mode is.
        if let Some(handle) = self.discoverable_timeout.take() {
            handle.abort();
        }

        if mode != BtDiscMode::NonDiscoverable && duration != 0 {
            let txl = self.tx.clone();
            self.discoverable_timeout = Some(tokio::spawn(async move {
                time::sleep(Duration::from_secs(duration.into())).await;
                let _ = txl.send(Message::AdapterActions(AdapterActions::ResetDiscoverable)).await;
            }));
        }

        true
    }

    fn is_multi_advertisement_supported(&self) -> bool {
        match self.properties.get(&BtPropertyType::LocalLeFeatures) {
            Some(prop) => match prop {
                BluetoothProperty::LocalLeFeatures(llf) => {
                    llf.max_adv_instance >= MIN_ADV_INSTANCES_FOR_MULTI_ADV
                }
                _ => false,
            },
            _ => false,
        }
    }

    fn is_le_extended_advertising_supported(&self) -> bool {
        match self.properties.get(&BtPropertyType::LocalLeFeatures) {
            Some(prop) => match prop {
                BluetoothProperty::LocalLeFeatures(llf) => llf.le_extended_advertising_supported,
                _ => false,
            },
            _ => false,
        }
    }

    fn start_discovery(&mut self) -> bool {
        // Short-circuit to avoid sending multiple start discovery calls.
        if self.is_discovering {
            return true;
        }

        // Short-circuit if paused and add the discovery intent to the queue.
        if self.is_discovery_paused {
            self.pending_discovery = true;
            debug!("Queue the discovery request during paused state");
            return true;
        }

        let discovery_suspend_mode = self.get_discovery_suspend_mode();
        if discovery_suspend_mode != SuspendMode::Normal
            && discovery_suspend_mode != SuspendMode::Resuming
        {
            log::warn!("start_discovery is not allowed when suspending or suspended.");
            return false;
        }

        self.intf.lock().unwrap().start_discovery() == 0
    }

    fn cancel_discovery(&mut self) -> bool {
        // Client no longer want to discover, clear the request
        if self.is_discovery_paused {
            self.pending_discovery = false;
            debug!("Cancel the discovery request during paused state");
        }

        // Reject the cancel discovery request if the underlying stack is not in a discovering
        // state. For example, previous start discovery was enqueued for ongoing discovery.
        if !self.is_discovering {
            debug!("Reject cancel_discovery as it's not in discovering state.");
            return false;
        }

        let discovery_suspend_mode = self.get_discovery_suspend_mode();
        if discovery_suspend_mode != SuspendMode::Normal
            && discovery_suspend_mode != SuspendMode::Suspending
        {
            log::warn!("cancel_discovery is not allowed when resuming or suspended.");
            return false;
        }

        self.intf.lock().unwrap().cancel_discovery() == 0
    }

    fn is_discovering(&self) -> bool {
        self.is_discovering
    }

    fn get_discovery_end_millis(&self) -> u64 {
        if !self.is_discovering {
            return 0;
        }

        let elapsed_ms = self.discovering_started.elapsed().as_millis() as u64;
        if elapsed_ms >= DEFAULT_DISCOVERY_TIMEOUT_MS {
            0
        } else {
            DEFAULT_DISCOVERY_TIMEOUT_MS - elapsed_ms
        }
    }

    fn create_bond(&mut self, device: BluetoothDevice, transport: BtTransport) -> BtStatus {
        let device_type = match transport {
            BtTransport::Bredr => BtDeviceType::Bredr,
            BtTransport::Le => BtDeviceType::Ble,
            _ => self.get_remote_type(device.clone()),
        };
        let address = device.address;

        if let Some(active_address) = self.active_pairing_address {
            warn!(
                "Bonding requested for {} while already bonding {}, rejecting",
                DisplayAddress(&address),
                DisplayAddress(&active_address)
            );
            return BtStatus::Busy;
        }

        if self.pending_create_bond.is_some() {
            warn!("Delayed CreateBond is still pending");
            return BtStatus::Busy;
        }

        // There could be a race between bond complete and bond cancel, which makes
        // |cancelling_devices| in a wrong state. Remove the device just in case.
        if self.cancelling_devices.remove(&address) {
            warn!("Device {} is also cancelling the bond.", DisplayAddress(&address));
        }

        // BREDR connection won't work when Inquiry / Remote Name Request is in progress.
        // If is_discovering, delay the request until discovery state change.
        if self.is_discovering {
            debug!("Discovering. Delay the CreateBond request until discovery is done.");
            self.pause_discovery();
            self.pending_create_bond = Some((device, transport));
            return BtStatus::Success;
        }

        // We explicitly log the attempt to start the bonding separate from logging the bond state.
        // The start of the attempt is critical to help identify a bonding/pairing session.
        metrics::bond_create_attempt(address, device_type.clone());

        self.active_pairing_address = Some(address);
        let status = self.intf.lock().unwrap().create_bond(&address, transport);

        if status != 0 {
            metrics::bond_state_changed(
                address,
                device_type,
                BtStatus::from(status as u32),
                BtBondState::NotBonded,
                0,
            );
            return BtStatus::from(status as u32);
        }

        // Creating bond automatically create ACL connection as well, therefore also log metrics
        // ACL connection attempt here.
        if !self.get_acl_state_by_addr(&address) {
            metrics::acl_connect_attempt(address, BtAclState::Connected);
        }

        BtStatus::Success
    }

    fn cancel_bond_process(&mut self, device: BluetoothDevice) -> bool {
        if !self.cancelling_devices.insert(device.address) {
            warn!(
                "Device {} has been added to cancelling_device.",
                DisplayAddress(&device.address)
            );
        }

        self.intf.lock().unwrap().cancel_bond(&device.address) == 0
    }

    fn remove_bond(&mut self, device: BluetoothDevice) -> bool {
        let address = device.address;

        // There could be a race between bond complete and bond cancel, which makes
        // |cancelling_devices| in a wrong state. Remove the device just in case.
        if self.cancelling_devices.remove(&address) {
            warn!("Device {} is also cancelling the bond.", DisplayAddress(&address));
        }

        let status = self.intf.lock().unwrap().remove_bond(&address);

        if status != 0 {
            return false;
        }

        // Removing bond also disconnects the ACL if is connected. Therefore, also log ACL
        // disconnection attempt here.
        if self.get_acl_state_by_addr(&address) {
            metrics::acl_connect_attempt(address, BtAclState::Disconnected);
        }

        true
    }

    fn get_bonded_devices(&self) -> Vec<BluetoothDevice> {
        self.remote_devices
            .values()
            .filter_map(|d| {
                if d.bond_state == BtBondState::Bonded {
                    Some(d.info.clone())
                } else {
                    None
                }
            })
            .collect()
    }

    fn get_bond_state(&self, device: BluetoothDevice) -> BtBondState {
        self.get_bond_state_by_addr(&device.address)
    }

    fn set_pin(&self, device: BluetoothDevice, accept: bool, pin_code: Vec<u8>) -> bool {
        if self.get_bond_state_by_addr(&device.address) != BtBondState::Bonding {
            warn!("Can't set pin. Device {} isn't bonding.", DisplayAddress(&device.address));
            return false;
        }

        let mut btpin = BtPinCode { pin: array_utils::to_sized_array(&pin_code) };

        self.intf.lock().unwrap().pin_reply(
            &device.address,
            accept as u8,
            pin_code.len() as u8,
            &mut btpin,
        ) == 0
    }

    fn set_passkey(&self, device: BluetoothDevice, accept: bool, passkey: Vec<u8>) -> bool {
        if self.get_bond_state_by_addr(&device.address) != BtBondState::Bonding {
            warn!("Can't set passkey. Device {} isn't bonding.", DisplayAddress(&device.address));
            return false;
        }

        let mut tmp: [u8; 4] = [0; 4];
        tmp.copy_from_slice(passkey.as_slice());
        let passkey = u32::from_ne_bytes(tmp);

        self.intf.lock().unwrap().ssp_reply(
            &device.address,
            BtSspVariant::PasskeyEntry,
            accept as u8,
            passkey,
        ) == 0
    }

    fn set_pairing_confirmation(&self, device: BluetoothDevice, accept: bool) -> bool {
        self.intf.lock().unwrap().ssp_reply(
            &device.address,
            BtSspVariant::PasskeyConfirmation,
            accept as u8,
            0,
        ) == 0
    }

    fn get_remote_name(&self, device: BluetoothDevice) -> String {
        match self.get_remote_device_property(&device, &BtPropertyType::BdName) {
            Some(BluetoothProperty::BdName(name)) => name.clone(),
            _ => "".to_string(),
        }
    }

    fn get_remote_type(&self, device: BluetoothDevice) -> BtDeviceType {
        match self.get_remote_device_property(&device, &BtPropertyType::TypeOfDevice) {
            Some(BluetoothProperty::TypeOfDevice(device_type)) => device_type,
            _ => BtDeviceType::Unknown,
        }
    }

    fn get_remote_alias(&self, device: BluetoothDevice) -> String {
        match self.get_remote_device_property(&device, &BtPropertyType::RemoteFriendlyName) {
            Some(BluetoothProperty::RemoteFriendlyName(name)) => name.clone(),
            _ => "".to_string(),
        }
    }

    fn set_remote_alias(&mut self, device: BluetoothDevice, new_alias: String) {
        let _ = self.set_remote_device_property(
            &device,
            BtPropertyType::RemoteFriendlyName,
            BluetoothProperty::RemoteFriendlyName(new_alias),
        );
    }

    fn get_remote_class(&self, device: BluetoothDevice) -> u32 {
        match self.get_remote_device_property(&device, &BtPropertyType::ClassOfDevice) {
            Some(BluetoothProperty::ClassOfDevice(class)) => class,
            _ => 0,
        }
    }

    fn get_remote_appearance(&self, device: BluetoothDevice) -> u16 {
        match self.get_remote_device_property(&device, &BtPropertyType::Appearance) {
            Some(BluetoothProperty::Appearance(appearance)) => appearance,
            _ => 0,
        }
    }

    fn get_remote_connected(&self, device: BluetoothDevice) -> bool {
        self.get_connection_state(device) != BtConnectionState::NotConnected
    }

    fn get_remote_wake_allowed(&self, device: BluetoothDevice) -> bool {
        // Wake is allowed if the device supports HIDP or HOGP only.
        match self.get_remote_device_property(&device, &BtPropertyType::Uuids) {
            Some(BluetoothProperty::Uuids(uuids)) => {
                return uuids.iter().any(|&uuid| {
                    UuidHelper::is_known_profile(&uuid).map_or(false, |profile| {
                        profile == Profile::Hid || profile == Profile::Hogp
                    })
                });
            }
            _ => false,
        }
    }

    fn get_remote_vendor_product_info(&self, device: BluetoothDevice) -> BtVendorProductInfo {
        match self.get_remote_device_property(&device, &BtPropertyType::VendorProductInfo) {
            Some(BluetoothProperty::VendorProductInfo(p)) => p,
            _ => BtVendorProductInfo { vendor_id_src: 0, vendor_id: 0, product_id: 0, version: 0 },
        }
    }

    fn get_remote_address_type(&self, device: BluetoothDevice) -> BtAddrType {
        match self.get_remote_device_property(&device, &BtPropertyType::RemoteAddrType) {
            Some(BluetoothProperty::RemoteAddrType(addr_type)) => addr_type,
            _ => BtAddrType::Unknown,
        }
    }

    fn get_remote_rssi(&self, device: BluetoothDevice) -> i8 {
        match self.get_remote_device_property(&device, &BtPropertyType::RemoteRssi) {
            Some(BluetoothProperty::RemoteRssi(rssi)) => rssi,
            _ => INVALID_RSSI,
        }
    }

    fn get_connected_devices(&self) -> Vec<BluetoothDevice> {
        self.remote_devices
            .values()
            .filter_map(|d| if d.is_connected() { Some(d.info.clone()) } else { None })
            .collect()
    }

    fn get_connection_state(&self, device: BluetoothDevice) -> BtConnectionState {
        // The underlying api adds whether this is ENCRYPTED_BREDR or ENCRYPTED_LE.
        // As long as it is non-zero, it is connected.
        self.intf.lock().unwrap().get_connection_state(&device.address)
    }

    fn get_profile_connection_state(&self, profile: Uuid) -> ProfileConnectionState {
        if let Some(known) = UuidHelper::is_known_profile(&profile) {
            match known {
                Profile::A2dpSink | Profile::A2dpSource => self
                    .bluetooth_media
                    .as_ref()
                    .map_or(ProfileConnectionState::Disconnected, |media| {
                        media.lock().unwrap().get_a2dp_connection_state()
                    }),
                Profile::Hfp | Profile::HfpAg => self
                    .bluetooth_media
                    .as_ref()
                    .map_or(ProfileConnectionState::Disconnected, |media| {
                        media.lock().unwrap().get_hfp_connection_state()
                    }),
                // TODO: (b/223431229) Profile::Hid and Profile::Hogp
                _ => ProfileConnectionState::Disconnected,
            }
        } else {
            ProfileConnectionState::Disconnected
        }
    }

    fn get_remote_uuids(&self, device: BluetoothDevice) -> Vec<Uuid> {
        match self.get_remote_device_property(&device, &BtPropertyType::Uuids) {
            Some(BluetoothProperty::Uuids(uuids)) => uuids,
            _ => vec![],
        }
    }

    fn fetch_remote_uuids(&self, remote_device: BluetoothDevice) -> bool {
        let Some(device) = self.remote_devices.get(&remote_device.address) else {
            warn!("Won't fetch UUIDs on unknown device");
            return false;
        };

        let transport = match self.get_remote_type(device.info.clone()) {
            BtDeviceType::Bredr => BtTransport::Bredr,
            BtDeviceType::Ble => BtTransport::Le,
            _ => device.acl_reported_transport,
        };

        self.intf.lock().unwrap().get_remote_services(&mut device.info.address.clone(), transport)
            == 0
    }

    fn sdp_search(&self, mut device: BluetoothDevice, uuid: Uuid) -> bool {
        if let Some(sdp) = self.sdp.as_ref() {
            return sdp.sdp_search(&mut device.address, &uuid) == BtStatus::Success;
        }
        false
    }

    fn create_sdp_record(&mut self, sdp_record: BtSdpRecord) -> bool {
        let mut handle: i32 = -1;
        let mut sdp_record = sdp_record;
        match self.sdp.as_ref().unwrap().create_sdp_record(&mut sdp_record, &mut handle) {
            BtStatus::Success => {
                let record_clone = sdp_record.clone();
                self.callbacks.for_all_callbacks(|callback| {
                    callback.on_sdp_record_created(record_clone.clone(), handle);
                });
                true
            }
            _ => false,
        }
    }

    fn remove_sdp_record(&self, handle: i32) -> bool {
        self.sdp.as_ref().unwrap().remove_sdp_record(handle) == BtStatus::Success
    }

    fn connect_all_enabled_profiles(&mut self, device: BluetoothDevice) -> BtStatus {
        // Profile init must be complete before this api is callable
        if !self.profiles_ready {
            return BtStatus::NotReady;
        }

        // Check all remote uuids to see if they match enabled profiles and connect them.
        let uuids = self.get_remote_uuids(device.clone());
        self.connect_profiles_internal(&uuids, device.clone());

        // Also connect to profiles discovered in the future.
        if let Some(d) = self.remote_devices.get_mut(&device.address) {
            d.connect_to_new_profiles = true;
        }

        BtStatus::Success
    }

    fn disconnect_all_enabled_profiles(&mut self, device: BluetoothDevice) -> bool {
        if !self.profiles_ready {
            return false;
        }
        let addr = device.address;

        // log ACL disconnection attempt if it's not already disconnected.
        if self.get_acl_state_by_addr(&addr) {
            metrics::acl_connect_attempt(addr, BtAclState::Disconnected);
        }

        let uuids = self.get_remote_uuids(device.clone());
        let mut has_classic_media_profile = false;
        let mut has_le_media_profile = false;
        for uuid in uuids.iter() {
            match UuidHelper::is_known_profile(uuid) {
                Some(p) => {
                    if UuidHelper::is_profile_supported(&p) {
                        match p {
                            Profile::Hid | Profile::Hogp => {
                                // TODO(b/328675014): Use BtAddrType
                                // and BtTransport from
                                // BluetoothDevice instead of default

                                // TODO(b/329837967): Determine
                                // correct reconnection behavior based
                                // on device instead of the default
                                self.hh.as_ref().unwrap().disconnect(
                                    &mut addr.clone(),
                                    BtAddrType::Public,
                                    BtTransport::Auto,
                                    /*reconnect_allowed=*/ true,
                                );
                            }

                            // TODO(b/317682584): implement policy to disconnect from LEA, VC, and CSIS
                            Profile::LeAudio | Profile::VolumeControl | Profile::CoordinatedSet
                                if !has_le_media_profile =>
                            {
                                has_le_media_profile = true;
                                let txl = self.tx.clone();
                                topstack::get_runtime().spawn(async move {
                                    let _ = txl
                                        .send(Message::Media(
                                            MediaActions::DisconnectLeaGroupByMemberAddress(addr),
                                        ))
                                        .await;
                                });
                            }

                            Profile::A2dpSink
                            | Profile::A2dpSource
                            | Profile::Hfp
                            | Profile::AvrcpController
                                if !has_classic_media_profile =>
                            {
                                has_classic_media_profile = true;
                                let txl = self.tx.clone();
                                topstack::get_runtime().spawn(async move {
                                    let _ = txl
                                        .send(Message::Media(MediaActions::Disconnect(addr)))
                                        .await;
                                });
                            }

                            // We don't connect most profiles
                            _ => (),
                        }
                    }
                }
                _ => {}
            }
        }

        // Disconnect all socket connections
        let txl = self.tx.clone();
        topstack::get_runtime().spawn(async move {
            let _ =
                txl.send(Message::SocketManagerActions(SocketActions::DisconnectAll(addr))).await;
        });

        // Disconnect all GATT connections
        let txl = self.tx.clone();
        topstack::get_runtime().spawn(async move {
            let _ = txl.send(Message::GattActions(GattActions::Disconnect(device))).await;
        });

        if let Some(d) = self.remote_devices.get_mut(&addr) {
            d.connect_to_new_profiles = false;
        }

        true
    }

    fn is_wbs_supported(&self) -> bool {
        self.intf.lock().unwrap().get_wbs_supported()
    }

    fn is_swb_supported(&self) -> bool {
        self.intf.lock().unwrap().get_swb_supported()
    }

    fn get_supported_roles(&self) -> Vec<BtAdapterRole> {
        let mut roles: Vec<BtAdapterRole> = vec![];

        // See Core 5.3, Vol 4, Part E, 7.8.27 for detailed state information
        if self.le_supported_states >> 35 & 1 == 1u64 {
            roles.push(BtAdapterRole::Central);
        }
        if self.le_supported_states >> 38 & 1 == 1u64 {
            roles.push(BtAdapterRole::Peripheral);
        }
        if self.le_supported_states >> 28 & 1 == 1u64 {
            roles.push(BtAdapterRole::CentralPeripheral);
        }

        roles
    }

    fn is_coding_format_supported(&self, coding_format: EscoCodingFormat) -> bool {
        self.intf.lock().unwrap().is_coding_format_supported(coding_format as u8)
    }

    fn is_le_audio_supported(&self) -> bool {
        // We determine LE Audio support by checking CIS Central support
        // See Core 5.3, Vol 6, 4.6 FEATURE SUPPORT
        self.le_local_supported_features >> 28 & 1 == 1u64
    }

    fn is_dual_mode_audio_sink_device(&self, device: BluetoothDevice) -> bool {
        fn is_dual_mode(uuids: Vec<Uuid>) -> bool {
            fn get_unwrapped_uuid(profile: Profile) -> Uuid {
                *UuidHelper::get_profile_uuid(&profile).unwrap_or(&Uuid::empty())
            }

            uuids.contains(&get_unwrapped_uuid(Profile::LeAudio))
                && (uuids.contains(&get_unwrapped_uuid(Profile::A2dpSink))
                    || uuids.contains(&get_unwrapped_uuid(Profile::Hfp)))
        }

        let Some(media) = self.bluetooth_media.as_ref() else {
            return false;
        };
        let media = media.lock().unwrap();
        let group_id = media.get_group_id(device.address);
        if group_id == LEA_UNKNOWN_GROUP_ID {
            return is_dual_mode(self.get_remote_uuids(device));
        }

        // Check if any device in the CSIP group is a dual mode audio sink device
        media.get_group_devices(group_id).iter().any(|addr| {
            is_dual_mode(self.get_remote_uuids(BluetoothDevice::new(*addr, "".to_string())))
        })
    }

    fn get_dumpsys(&self) -> String {
        OpenOptions::new()
            .write(true)
            .create(true)
            .truncate(true)
            .open(DUMPSYS_LOG)
            .and_then(|file| {
                let fd = file.as_raw_fd();
                self.intf.lock().unwrap().dump(fd);
                Ok(format!("dump to {}", DUMPSYS_LOG))
            })
            .unwrap_or_default()
    }
}

impl BtifSdpCallbacks for Bluetooth {
    fn sdp_search(
        &mut self,
        status: BtStatus,
        address: RawAddress,
        uuid: Uuid,
        _count: i32,
        records: Vec<BtSdpRecord>,
    ) {
        let device_info = match self.remote_devices.get(&address) {
            Some(d) => d.info.clone(),
            None => BluetoothDevice::new(address, "".to_string()),
        };

        // The SDP records we get back do not populate the UUID so we populate it ourselves before
        // sending them on.
        let mut records = records;
        records.iter_mut().for_each(|record| {
            match record {
                BtSdpRecord::HeaderOverlay(header) => header.uuid = uuid,
                BtSdpRecord::MapMas(record) => record.hdr.uuid = uuid,
                BtSdpRecord::MapMns(record) => record.hdr.uuid = uuid,
                BtSdpRecord::PbapPse(record) => record.hdr.uuid = uuid,
                BtSdpRecord::PbapPce(record) => record.hdr.uuid = uuid,
                BtSdpRecord::OppServer(record) => record.hdr.uuid = uuid,
                BtSdpRecord::SapServer(record) => record.hdr.uuid = uuid,
                BtSdpRecord::Dip(record) => record.hdr.uuid = uuid,
                BtSdpRecord::Mps(record) => record.hdr.uuid = uuid,
            };
        });
        self.callbacks.for_all_callbacks(|callback| {
            callback.on_sdp_search_complete(device_info.clone(), uuid, records.clone());
        });
        debug!(
            "Sdp search result found: Status={:?} Address={} Uuid={}",
            status,
            DisplayAddress(&address),
            DisplayUuid(&uuid)
        );
    }
}

impl BtifHHCallbacks for Bluetooth {
    fn connection_state(
        &mut self,
        address: RawAddress,
        address_type: BtAddrType,
        transport: BtTransport,
        state: BthhConnectionState,
    ) {
        debug!(
            "Hid host connection state updated: Address({}) State({:?})",
            DisplayAddress(&address),
            state
        );

        // HID or HOG is not differentiated by the hid host when callback this function. Assume HOG
        // if the device is LE only and HID if classic only. And assume HOG if UUID said so when
        // device type is dual or unknown.
        let device = BluetoothDevice::new(address, "".to_string());
        let profile = match self.get_remote_type(device.clone()) {
            BtDeviceType::Ble => Profile::Hogp,
            BtDeviceType::Bredr => Profile::Hid,
            _ => {
                if self
                    .get_remote_uuids(device)
                    .contains(UuidHelper::get_profile_uuid(&Profile::Hogp).unwrap())
                {
                    Profile::Hogp
                } else {
                    Profile::Hid
                }
            }
        };

        metrics::profile_connection_state_changed(
            address,
            profile as u32,
            BtStatus::Success,
            state as u32,
        );

        let tx = self.tx.clone();
        self.remote_devices.entry(address).and_modify(|context| {
            if context.is_hh_connected && state != BthhConnectionState::Connected {
                tokio::spawn(async move {
                    let _ = tx.send(Message::ProfileDisconnected(address)).await;
                });
            }
            context.is_hh_connected = state == BthhConnectionState::Connected;
        });

        if BtBondState::Bonded != self.get_bond_state_by_addr(&address)
            && (state != BthhConnectionState::Disconnecting
                && state != BthhConnectionState::Disconnected)
        {
            warn!(
                "[{}]: Rejecting a unbonded device's attempt to connect to HID/HOG profiles",
                DisplayAddress(&address)
            );
            // TODO(b/329837967): Determine correct reconnection
            // behavior based on device instead of the default
            let mut address = address;
            self.hh.as_ref().unwrap().disconnect(
                &mut address,
                address_type,
                transport,
                /*reconnect_allowed=*/ true,
            );
        }
    }

    fn hid_info(
        &mut self,
        address: RawAddress,
        address_type: BtAddrType,
        transport: BtTransport,
        info: BthhHidInfo,
    ) {
        debug!(
            "Hid host info updated: Address({}) AddressType({:?}) Transport({:?}) Info({:?})",
            DisplayAddress(&address),
            address_type,
            transport,
            info
        );
    }

    fn protocol_mode(
        &mut self,
        address: RawAddress,
        address_type: BtAddrType,
        transport: BtTransport,
        status: BthhStatus,
        mode: BthhProtocolMode,
    ) {
        debug!(
            "Hid host protocol mode updated: Address({}) AddressType({:?}) Transport({:?}) Status({:?}) Mode({:?})",
            DisplayAddress(&address), address_type, transport,
            status,
            mode
        );
    }

    fn idle_time(
        &mut self,
        address: RawAddress,
        address_type: BtAddrType,
        transport: BtTransport,
        status: BthhStatus,
        idle_rate: i32,
    ) {
        debug!(
            "Hid host idle time updated: Address({}) AddressType({:?}) Transport({:?}) Status({:?}) Idle Rate({:?})",
            DisplayAddress(&address), address_type, transport,
            status,
            idle_rate
        );
    }

    fn get_report(
        &mut self,
        address: RawAddress,
        address_type: BtAddrType,
        transport: BtTransport,
        status: BthhStatus,
        _data: Vec<u8>,
        size: i32,
    ) {
        debug!(
            "Hid host got report: Address({}) AddressType({:?}) Transport({:?}) Status({:?}) Report Size({:?})",
            DisplayAddress(&address), address_type, transport,
            status,
            size
        );
    }

    fn handshake(
        &mut self,
        address: RawAddress,
        address_type: BtAddrType,
        transport: BtTransport,
        status: BthhStatus,
    ) {
        debug!(
            "Hid host handshake: Address({}) AddressType({:?}) Transport({:?}) Status({:?})",
            DisplayAddress(&address),
            address_type,
            transport,
            status
        );
    }
}

// TODO(b/261143122): Remove these once we migrate to BluetoothQA entirely
impl IBluetoothQALegacy for Bluetooth {
    fn get_connectable(&self) -> bool {
        self.get_connectable_internal()
    }

    fn set_connectable(&mut self, mode: bool) -> bool {
        self.set_connectable_internal(mode)
    }

    fn get_alias(&self) -> String {
        self.get_alias_internal()
    }

    fn get_modalias(&self) -> String {
        format!("bluetooth:v00E0pC405d{:04x}", FLOSS_VER)
    }

    fn get_hid_report(
        &mut self,
        addr: RawAddress,
        report_type: BthhReportType,
        report_id: u8,
    ) -> BtStatus {
        self.get_hid_report_internal(addr, report_type, report_id)
    }

    fn set_hid_report(
        &mut self,
        addr: RawAddress,
        report_type: BthhReportType,
        report: String,
    ) -> BtStatus {
        self.set_hid_report_internal(addr, report_type, report)
    }

    fn send_hid_data(&mut self, addr: RawAddress, data: String) -> BtStatus {
        self.send_hid_data_internal(addr, data)
    }
}
