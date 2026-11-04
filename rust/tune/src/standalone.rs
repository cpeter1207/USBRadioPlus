//! Bounded local control transport for the standalone radio service.
//!
//! Clones share one persistent connection and therefore one maintenance lease.
//! Any transport/protocol failure closes that connection to release temporary
//! keying and tones. Ordinary service rejections retain the connection so the
//! frontend can restore configuration and release its manual controls.
//! Requests never contain a configuration pathname.

use std::cell::RefCell;
use std::io::{Read, Write};
use std::os::unix::net::UnixStream;
use std::path::{Path, PathBuf};
use std::rc::Rc;
use std::time::Duration;

use serde_json::{Value, json};
use usbradioplus_core::{CtcssTone, radio_control::*};

use crate::control::parse_radio_status;
use crate::menus::{
    ApplyMode, ChannelControl, ConfigurationApplier, HardwareMixer, LiveAction, RadioStatus,
};

const VERSION: u32 = 1;
const MAX_FRAME: usize = 65_536;

/// Typed tuner backend sharing the standalone service's session ownership.
#[derive(Clone, Debug)]
pub struct StandaloneRuntime(Rc<RefCell<Session>>);

#[derive(Debug)]
struct Session {
    stream: Option<UnixStream>,
    node: Option<String>,
}

impl StandaloneRuntime {
    /// Connect once to the service-owned socket with bounded I/O waits.
    ///
    /// # Errors
    /// Returns an error when connection or timeout configuration fails.
    pub fn connect(path: &Path) -> Result<Self, String> {
        let stream = UnixStream::connect(path)
            .map_err(|error| format!("connect {}: {error}", path.display()))?;
        stream
            .set_read_timeout(Some(Duration::from_secs(10)))
            .map_err(|error| error.to_string())?;
        stream
            .set_write_timeout(Some(Duration::from_secs(10)))
            .map_err(|error| error.to_string())?;
        Ok(Self(Rc::new(RefCell::new(Session {
            stream: Some(stream),
            node: None,
        }))))
    }

    /// Read the service's fixed canonical configuration path before editing.
    ///
    /// # Errors
    /// Returns an error when the service rejects or malforms its response.
    pub fn configuration_path(&self) -> Result<PathBuf, String> {
        self.text_request("configuration-path").map(PathBuf::from)
    }

    fn request(&self, mut request: Value) -> Result<Value, String> {
        let mut session = self.0.borrow_mut();
        request["version"] = json!(VERSION);
        if !matches!(
            request["operation"].as_str(),
            Some("nodes" | "configuration-path")
        ) {
            let node = session
                .node
                .as_ref()
                .ok_or_else(|| "select a standalone radio node first".to_owned())?;
            request["node"] = json!(node);
        }
        let result = session
            .stream
            .as_mut()
            .ok_or_else(|| "standalone control session is disconnected".to_owned())
            .and_then(|stream| exchange(stream, &request));
        match result {
            Ok(application_result) => application_result,
            Err(error) => {
                session.stream.take();
                Err(error)
            }
        }
    }

    fn text_request(&self, operation: &str) -> Result<String, String> {
        let result = self.request(json!({"operation": operation}))?;
        result
            .as_str()
            .map(str::to_owned)
            .ok_or_else(|| self.invalid_reply("expected text result"))
    }

    fn invalid_reply(&self, message: &str) -> String {
        self.0.borrow_mut().stream.take();
        format!("invalid standalone control response: {message}")
    }

    fn command(&self, command: u32, target: u32, value: i64) -> Result<Value, String> {
        let result = self.request(json!({"operation":"command", "command": command, "target": target, "value": value, "flags": 0}))?;
        if result["command"].as_u64() != Some(u64::from(command))
            || result["target"].as_u64() != Some(u64::from(target))
            || result["value"].as_i64().is_none()
            || result["flags"]
                .as_u64()
                .is_none_or(|flags| u32::try_from(flags).is_err())
        {
            return Err(self.invalid_reply("expected command result"));
        }
        Ok(result)
    }

    fn unit_request(&self, request: Value) -> Result<(), String> {
        if self.request(request)?.is_null() {
            Ok(())
        } else {
            Err(self.invalid_reply("expected empty result"))
        }
    }

    fn nodes(&self) -> Result<Vec<String>, String> {
        let result = self.request(json!({"operation":"nodes"}))?;
        serde_json::from_value(result).map_err(|_| self.invalid_reply("expected node list"))
    }
}

fn exchange(stream: &mut UnixStream, request: &Value) -> Result<Result<Value, String>, String> {
    let bytes = request.to_string().into_bytes();
    if bytes.len() > MAX_FRAME {
        return Err("standalone control request exceeds frame limit".to_owned());
    }
    stream
        .write_all(&(bytes.len() as u32).to_be_bytes())
        .and_then(|()| stream.write_all(&bytes))
        .map_err(|error| format!("write standalone control request: {error}"))?;
    let mut prefix = [0; 4];
    stream
        .read_exact(&mut prefix)
        .map_err(|error| format!("read standalone control response: {error}"))?;
    let length = u32::from_be_bytes(prefix) as usize;
    if length == 0 || length > MAX_FRAME {
        return Err("invalid standalone control response frame length".to_owned());
    }
    let mut bytes = vec![0; length];
    stream
        .read_exact(&mut bytes)
        .map_err(|error| format!("read standalone control response: {error}"))?;
    let reply: Value = serde_json::from_slice(&bytes)
        .map_err(|error| format!("invalid standalone control JSON: {error}"))?;
    if reply["version"].as_u64() != Some(u64::from(VERSION)) {
        return Err("standalone control protocol version mismatch".to_owned());
    }
    match reply["ok"].as_bool() {
        Some(true) => reply
            .get("result")
            .cloned()
            .map(Ok)
            .ok_or_else(|| "standalone control response omits result".to_owned()),
        Some(false) => reply["error"]
            .as_str()
            .map(|error| Err(error.to_owned()))
            .ok_or_else(|| "standalone control response omits error".to_owned()),
        None => Err("standalone control response omits outcome".to_owned()),
    }
}

impl ConfigurationApplier for StandaloneRuntime {
    fn apply(&mut self, _mode: ApplyMode) -> Result<(), String> {
        self.unit_request(json!({"operation":"reload"}))
    }
}

impl ChannelControl for StandaloneRuntime {
    fn active_channel(&mut self) -> Result<Option<String>, String> {
        let current = self.0.borrow().node.clone();
        if current.is_some() {
            return Ok(current);
        }
        let selected = self.nodes()?.into_iter().next();
        self.0.borrow_mut().node = selected.clone();
        Ok(selected)
    }

    fn select_channel(&mut self, channel: &str) -> Result<(), String> {
        if !self.nodes()?.iter().any(|node| node == channel) {
            return Err(format!("standalone radio node {channel} is unavailable"));
        }
        self.0.borrow_mut().node = Some(channel.to_owned());
        Ok(())
    }

    fn radio_status(&mut self) -> Result<RadioStatus, String> {
        parse_radio_status(&self.text_request("status")?)
            .map_err(|error| self.invalid_reply(&error))
    }

    fn set_hardware_mixer(&mut self, mixer: HardwareMixer, level: u32) -> Result<(), String> {
        let target = match mixer {
            HardwareMixer::Receive => URP_AST_MIXER_RECEIVE,
            HardwareMixer::TransmitA => URP_AST_MIXER_TRANSMIT_A,
            HardwareMixer::TransmitB => URP_AST_MIXER_TRANSMIT_B,
        };
        self.command(URP_AST_COMMAND_SET_MIXER, target, i64::from(level))
            .map(|_| ())
    }

    fn set_test_tone(&mut self, enabled: bool) -> Result<(), String> {
        self.command(URP_AST_COMMAND_SET_TEST_TONE, 0, i64::from(enabled))
            .map(|_| ())
    }

    fn set_transmit(&mut self, keyed: bool, forced_ctcss: Option<CtcssTone>) -> Result<(), String> {
        self.unit_request(json!({"operation":"transmit", "keyed":keyed, "forced_ctcss_tenths_hz":forced_ctcss.map_or(0, CtcssTone::tenths_hz)}))
    }

    fn save_current_tuning_to_eeprom(&mut self) -> Result<String, String> {
        let result = self.command(URP_AST_COMMAND_SAVE_TUNING_EEPROM, 0, 0)?;
        match result["value"].as_i64() {
            Some(1) => Ok("Live radio tuning saved to EEPROM.".to_owned()),
            Some(0) => Err("EEPROM tuning storage is disabled.".to_owned()),
            _ => Err(self.invalid_reply("invalid EEPROM save result")),
        }
    }

    fn wait(&mut self, duration: Duration) {
        std::thread::sleep(duration);
    }

    fn perform(&mut self, action: LiveAction) -> Result<String, String> {
        match action {
            LiveAction::ProcessingStatistics => self.text_request("processing-stats"),
            LiveAction::ActiveConfiguration => self.text_request("active-configuration"),
            LiveAction::RadioStatus => self.text_request("status"),
            LiveAction::SaveRadioTuning => self.save_current_tuning_to_eeprom(),
            LiveAction::FlashTransmitter
            | LiveAction::ContinuousRadioMeters
            | LiveAction::CalibrateReceiveNoise
            | LiveAction::CalibrateReceiveVoice
            | LiveAction::CalibrateReceiveCtcss => {
                Err("the tuner composes calibration from typed live controls".to_owned())
            }
        }
    }

    fn echo_enabled(&mut self) -> Result<bool, String> {
        Err("echo playback is unavailable in standalone mode".to_owned())
    }
    fn set_echo_enabled(&mut self, _enabled: bool) -> Result<String, String> {
        Err("echo playback is unavailable in standalone mode".to_owned())
    }
}

#[cfg(test)]
#[cfg_attr(coverage, coverage(off))]
pub(crate) mod tests {
    use super::*;

    const STATUS: &str = "receive_input_peak=0.75\nreceive_input_rms=0.25\nreceive_output_peak=0.5\nreceive_output_rms=0.125\nreceive_ctcss_decoder_peak=0.1\nreceive_rssi_peak=20000\nreceive_rssi_updated=1\nreceive_input_rail_samples=0\nreceive_mixer_level=500\n";

    fn receive(stream: &mut UnixStream) -> Value {
        let mut length = [0; 4];
        stream.read_exact(&mut length).unwrap();
        let mut bytes = vec![0; u32::from_be_bytes(length) as usize];
        stream.read_exact(&mut bytes).unwrap();
        serde_json::from_slice(&bytes).unwrap()
    }

    fn send(stream: &mut UnixStream, reply: &Value) {
        let bytes = serde_json::to_vec(reply).unwrap();
        stream
            .write_all(&(bytes.len() as u32).to_be_bytes())
            .unwrap();
        stream.write_all(&bytes).unwrap();
    }

    fn runtime(stream: UnixStream) -> StandaloneRuntime {
        stream
            .set_read_timeout(Some(Duration::from_secs(2)))
            .unwrap();
        StandaloneRuntime(Rc::new(RefCell::new(Session {
            stream: Some(stream),
            node: None,
        })))
    }

    pub(crate) fn calibration_runtime() -> (StandaloneRuntime, std::thread::JoinHandle<Vec<Value>>)
    {
        calibration_runtime_with_rejected_reload(false)
    }

    pub(crate) fn calibration_runtime_with_rejected_reload(
        mut reject_reload: bool,
    ) -> (StandaloneRuntime, std::thread::JoinHandle<Vec<Value>>) {
        let (client, mut server) = UnixStream::pair().unwrap();
        let handle = std::thread::spawn(move || {
            let mut requests = Vec::new();
            let mut mixer = 500_u64;
            loop {
                let mut prefix = [0; 4];
                match server.read_exact(&mut prefix) {
                    Ok(()) => {}
                    Err(error) if error.kind() == std::io::ErrorKind::UnexpectedEof => break,
                    Err(error) => panic!("calibration socket failed: {error}"),
                }
                let mut bytes = vec![0; u32::from_be_bytes(prefix) as usize];
                server.read_exact(&mut bytes).unwrap();
                let request: Value = serde_json::from_slice(&bytes).unwrap();
                if request["operation"] == "reload" && reject_reload {
                    reject_reload = false;
                    send(
                        &mut server,
                        &json!({"version":1,"ok":false,"error":"candidate rejected"}),
                    );
                    requests.push(request);
                    continue;
                }
                let result = match request["operation"].as_str().unwrap() {
                    "nodes" => json!(["1000"]),
                    "reload" | "transmit" => Value::Null,
                    "command" => {
                        if request["command"] == 2 && request["target"] == 1 {
                            mixer = request["value"].as_u64().unwrap();
                        }
                        let value = if request["command"] == 9 {
                            json!(1)
                        } else {
                            request["value"].clone()
                        };
                        json!({"command":request["command"],"target":request["target"],"flags":0,"value":value})
                    }
                    "status" => json!(format!(
                        "receive_input_peak={}\nreceive_input_rms=0.25\nreceive_output_peak={}\nreceive_output_rms=0.125\nreceive_ctcss_decoder_peak={}\nreceive_rssi_peak=20000\nreceive_rssi_updated=1\nreceive_input_rail_samples=0\nreceive_mixer_level={mixer}\n",
                        (27000.0 / 32768.0) * (mixer as f32 / 500.0),
                        7200.0 / 32768.0,
                        2400.0 / 32768.0
                    )),
                    operation => panic!("unexpected calibration operation {operation}"),
                };
                send(&mut server, &json!({"version":1,"ok":true,"result":result}));
                requests.push(request);
            }
            requests
        });
        (runtime(client), handle)
    }

    #[test]
    fn persistent_socket_maps_typed_controls_and_shared_status_without_shell_commands() {
        let (client, mut server) = UnixStream::pair().unwrap();
        let handle = std::thread::spawn(move || {
            let cases = [
                (
                    json!({"version":1,"operation":"configuration-path"}),
                    json!("/etc/rpt_advanced/rpt_advanced.conf"),
                ),
                (json!({"version":1,"operation":"nodes"}), json!(["1000"])),
                (
                    json!({"version":1,"operation":"command","node":"1000","command":2,"target":1,"value":999,"flags":0}),
                    json!({"command":2,"target":1,"value":999,"flags":0}),
                ),
                (
                    json!({"version":1,"operation":"command","node":"1000","command":2,"target":2,"value":250,"flags":0}),
                    json!({"command":2,"target":2,"value":250,"flags":0}),
                ),
                (
                    json!({"version":1,"operation":"command","node":"1000","command":2,"target":3,"value":500,"flags":0}),
                    json!({"command":2,"target":3,"value":500,"flags":0}),
                ),
                (
                    json!({"version":1,"operation":"command","node":"1000","command":3,"target":0,"value":1,"flags":0}),
                    json!({"command":3,"target":0,"value":1,"flags":0}),
                ),
                (
                    json!({"version":1,"operation":"transmit","node":"1000","keyed":true,"forced_ctcss_tenths_hz":1000}),
                    Value::Null,
                ),
                (
                    json!({"version":1,"operation":"status","node":"1000"}),
                    json!(STATUS),
                ),
                (
                    json!({"version":1,"operation":"processing-stats","node":"1000"}),
                    json!("processed_blocks=42\n"),
                ),
                (
                    json!({"version":1,"operation":"active-configuration","node":"1000"}),
                    json!("[radio]\n"),
                ),
                (
                    json!({"version":1,"operation":"command","node":"1000","command":9,"target":0,"value":0,"flags":0}),
                    json!({"command":9,"target":0,"value":1,"flags":0}),
                ),
                (
                    json!({"version":1,"operation":"reload","node":"1000"}),
                    Value::Null,
                ),
                (
                    json!({"version":1,"operation":"reload","node":"1000"}),
                    Value::Null,
                ),
                (
                    json!({"version":1,"operation":"transmit","node":"1000","keyed":false,"forced_ctcss_tenths_hz":0}),
                    Value::Null,
                ),
                (
                    json!({"version":1,"operation":"command","node":"1000","command":3,"target":0,"value":0,"flags":0}),
                    json!({"command":3,"target":0,"value":0,"flags":0}),
                ),
            ];
            for (expected, result) in cases {
                assert_eq!(receive(&mut server), expected);
                send(&mut server, &json!({"version":1,"ok":true,"result":result}));
            }
            let mut byte = [0];
            assert_eq!(
                server.read(&mut byte).unwrap(),
                0,
                "dropping the last clone must release its maintenance lease"
            );
        });
        let mut runtime = runtime(client);
        let mut applier = runtime.clone();
        assert_eq!(
            runtime.configuration_path().unwrap(),
            PathBuf::from("/etc/rpt_advanced/rpt_advanced.conf")
        );
        runtime.select_channel("1000").unwrap();
        assert_eq!(runtime.active_channel().unwrap().as_deref(), Some("1000"));
        runtime
            .set_hardware_mixer(HardwareMixer::Receive, 999)
            .unwrap();
        runtime
            .set_hardware_mixer(HardwareMixer::TransmitA, 250)
            .unwrap();
        runtime
            .set_hardware_mixer(HardwareMixer::TransmitB, 500)
            .unwrap();
        runtime.set_test_tone(true).unwrap();
        runtime
            .set_transmit(true, Some("100.0".parse().unwrap()))
            .unwrap();
        let status = runtime.radio_status().unwrap();
        assert_eq!(status.receive_output_peak, 0.5);
        assert_eq!(status.receive_rssi_peak, 20_000);
        assert!(status.receive_rssi_updated);
        assert_eq!(
            runtime.perform(LiveAction::ProcessingStatistics).unwrap(),
            "processed_blocks=42\n"
        );
        assert_eq!(
            runtime.perform(LiveAction::ActiveConfiguration).unwrap(),
            "[radio]\n"
        );
        assert!(
            runtime
                .save_current_tuning_to_eeprom()
                .unwrap()
                .contains("saved")
        );
        applier.apply(ApplyMode::Reload).unwrap();
        applier.apply(ApplyMode::Restart).unwrap();
        drop(applier);
        runtime.set_transmit(false, None).unwrap();
        runtime.set_test_tone(false).unwrap();
        drop(runtime);
        handle.join().unwrap();
    }

    #[test]
    fn malformed_responses_close_the_shared_session() {
        let replies = [
            vec![0, 1, 0, 1],
            vec![0, 0, 0, 0],
            vec![0, 0, 0, 1, b'{'],
            serde_json::to_vec(&json!({"version":2,"ok":true,"result":"bad"})).unwrap(),
            serde_json::to_vec(&json!({"version":1,"ok":false})).unwrap(),
            serde_json::to_vec(&json!({"version":1,"ok":false,"error":42})).unwrap(),
            serde_json::to_vec(&json!({"version":1})).unwrap(),
            serde_json::to_vec(&json!({"version":1,"ok":true})).unwrap(),
            serde_json::to_vec(&json!({"version":1,"ok":true,"result":42})).unwrap(),
            serde_json::to_vec(&json!({"version":1,"ok":true,"result":"invalid status"})).unwrap(),
        ];
        for reply in replies {
            let (client, mut server) = UnixStream::pair().unwrap();
            let handle = std::thread::spawn(move || {
                receive(&mut server);
                if reply.first() == Some(&b'{') {
                    server
                        .write_all(&(reply.len() as u32).to_be_bytes())
                        .unwrap();
                }
                server.write_all(&reply).unwrap();
                let mut byte = [0];
                assert_eq!(server.read(&mut byte).unwrap(), 0);
            });
            let mut runtime = runtime(client);
            runtime.0.borrow_mut().node = Some("1000".to_owned());
            let mut clone = runtime.clone();
            assert!(runtime.radio_status().is_err());
            assert!(
                clone
                    .set_transmit(false, None)
                    .unwrap_err()
                    .contains("disconnected")
            );
            handle.join().unwrap();
        }
    }

    #[test]
    fn coverage_inventory_selection_and_local_unsupported_controls_keep_connection() {
        let (client, mut server) = UnixStream::pair().unwrap();
        let handle = std::thread::spawn(move || {
            for nodes in [json!([]), json!(["1000"]), json!(["1000"]), json!(["1000"])] {
                assert_eq!(receive(&mut server)["operation"], "nodes");
                send(&mut server, &json!({"version":1,"ok":true,"result":nodes}));
            }
            let mut byte = [0];
            assert_eq!(server.read(&mut byte).unwrap(), 0);
        });
        let mut runtime = runtime(client);
        assert!(runtime.radio_status().unwrap_err().contains("select"));
        assert_eq!(runtime.active_channel().unwrap(), None);
        assert_eq!(runtime.active_channel().unwrap().as_deref(), Some("1000"));
        assert!(
            runtime
                .select_channel("missing")
                .unwrap_err()
                .contains("unavailable")
        );
        runtime.select_channel("1000").unwrap();
        for action in [
            LiveAction::FlashTransmitter,
            LiveAction::ContinuousRadioMeters,
            LiveAction::CalibrateReceiveNoise,
            LiveAction::CalibrateReceiveVoice,
            LiveAction::CalibrateReceiveCtcss,
        ] {
            assert!(
                runtime
                    .perform(action)
                    .unwrap_err()
                    .contains("typed live controls")
            );
        }
        assert!(runtime.echo_enabled().unwrap_err().contains("unavailable"));
        assert!(
            runtime
                .set_echo_enabled(true)
                .unwrap_err()
                .contains("unavailable")
        );
        drop(runtime);
        handle.join().unwrap();
    }

    #[test]
    fn coverage_command_validation_closes_invalid_typed_responses() {
        for result in [
            json!({"command":3,"target":1,"value":1,"flags":0}),
            json!({"command":2,"target":2,"value":1,"flags":0}),
            json!({"command":2,"target":1,"value":"bad","flags":0}),
            json!({"command":2,"target":1,"value":1}),
            json!({"command":2,"target":1,"value":1,"flags":4294967296_u64}),
        ] {
            let (client, mut server) = UnixStream::pair().unwrap();
            let handle = std::thread::spawn(move || {
                assert_eq!(receive(&mut server)["operation"], "command");
                send(&mut server, &json!({"version":1,"ok":true,"result":result}));
                let mut byte = [0];
                assert_eq!(server.read(&mut byte).unwrap(), 0);
            });
            let mut runtime = runtime(client);
            runtime.0.borrow_mut().node = Some("1000".to_owned());
            assert!(
                runtime
                    .set_hardware_mixer(HardwareMixer::Receive, 1)
                    .unwrap_err()
                    .contains("command result")
            );
            assert!(
                runtime
                    .set_test_tone(false)
                    .unwrap_err()
                    .contains("disconnected")
            );
            handle.join().unwrap();
        }
    }

    #[test]
    fn coverage_unit_node_and_eeprom_result_shapes_are_enforced() {
        for (operation, result, message) in [
            ("transmit", json!(true), "empty result"),
            ("nodes", json!([1]), "node list"),
            (
                "command",
                json!({"command":9,"target":0,"value":2,"flags":0}),
                "EEPROM save result",
            ),
        ] {
            let (client, mut server) = UnixStream::pair().unwrap();
            let handle = std::thread::spawn(move || {
                assert_eq!(receive(&mut server)["operation"], operation);
                send(&mut server, &json!({"version":1,"ok":true,"result":result}));
                let mut byte = [0];
                assert_eq!(server.read(&mut byte).unwrap(), 0);
            });
            let mut runtime = runtime(client);
            runtime.0.borrow_mut().node = Some("1000".to_owned());
            let error = match operation {
                "transmit" => runtime.set_transmit(false, None).unwrap_err(),
                "nodes" => runtime.select_channel("1000").unwrap_err(),
                _ => runtime.save_current_tuning_to_eeprom().unwrap_err(),
            };
            assert!(error.contains(message), "{error}");
            assert!(
                runtime
                    .set_test_tone(false)
                    .unwrap_err()
                    .contains("disconnected")
            );
            handle.join().unwrap();
        }
        let (client, mut server) = UnixStream::pair().unwrap();
        let handle = std::thread::spawn(move || {
            for value in [0, 1] {
                assert_eq!(receive(&mut server)["command"], 9);
                send(
                    &mut server,
                    &json!({"version":1,"ok":true,"result":{"command":9,"target":0,"value":value,"flags":0}}),
                );
            }
        });
        let mut runtime = runtime(client);
        runtime.0.borrow_mut().node = Some("1000".to_owned());
        assert!(
            runtime
                .save_current_tuning_to_eeprom()
                .unwrap_err()
                .contains("disabled")
        );
        assert!(
            runtime
                .perform(LiveAction::SaveRadioTuning)
                .unwrap()
                .contains("saved")
        );
        handle.join().unwrap();
    }

    #[test]
    fn coverage_oversized_request_and_peer_loss_disconnect_all_clones() {
        let (client, mut server) = UnixStream::pair().unwrap();
        let handle = std::thread::spawn(move || {
            assert_eq!(receive(&mut server)["operation"], "nodes");
            send(
                &mut server,
                &json!({"version":1,"ok":true,"result":["n".repeat(MAX_FRAME - 60)]}),
            );
            let mut byte = [0];
            assert_eq!(server.read(&mut byte).unwrap(), 0);
        });
        let mut runtime = runtime(client);
        assert!(runtime.active_channel().unwrap().is_some());
        assert!(
            runtime
                .set_test_tone(true)
                .unwrap_err()
                .contains("frame limit")
        );
        assert!(
            runtime
                .set_transmit(false, None)
                .unwrap_err()
                .contains("disconnected")
        );
        handle.join().unwrap();
        let (client, server) = UnixStream::pair().unwrap();
        drop(server);
        let runtime = self::runtime(client);
        assert!(runtime.configuration_path().unwrap_err().contains("write"));
        assert!(
            runtime
                .configuration_path()
                .unwrap_err()
                .contains("disconnected")
        );
    }
}
