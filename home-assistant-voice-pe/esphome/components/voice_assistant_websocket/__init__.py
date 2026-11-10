import os

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import automation
from esphome.automation import maybe_simple_id
from esphome.components import microphone, speaker
from esphome.const import CONF_ID, CONF_MICROPHONE, CONF_SPEAKER
from esphome.core import CORE
from esphome.components.esp32 import add_idf_component

CODEOWNERS = ["@openai-realtime-voice-agent"]
DEPENDENCIES = ["microphone", "speaker"]
AUTO_LOAD = ["json"]

voice_assistant_websocket_ns = cg.esphome_ns.namespace("voice_assistant_websocket")
VoiceAssistantWebSocket = voice_assistant_websocket_ns.class_(
    "VoiceAssistantWebSocket", cg.Component
)

CONF_ENROLLMENT_WAKE_CHANNEL = "enrollment_wake_channel"
CONF_ENROLLMENT_WAKE_GAIN = "enrollment_wake_gain_factor"
CONF_SERVER_URL = "server_url"
# Wake-trigger capture metadata (wake_trigger_capture.h): facts about the detector sent with each
# captured trigger. Channel/gain are NOT repeated here - they reuse enrollment_wake_* above, i.e.
# exactly what micro_wake_word consumes.
CONF_WAKE_TRIGGER_MODEL = "wake_trigger_model"
CONF_WAKE_TRIGGER_FW = "wake_trigger_fw"
CONF_WAKE_TRIGGER_WINDOW = "wake_trigger_window"
CONF_VOICE_ASSISTANT_WEBSOCKET = "voice_assistant_websocket"
CONF_ON_CONNECTED = "on_connected"
CONF_ON_DISCONNECTED = "on_disconnected"
CONF_ON_ERROR = "on_error"
CONF_ON_STOPPED = "on_stopped"
CONF_ON_ENROLL_START = "on_enroll_start"
CONF_ON_ENROLL_STOP = "on_enroll_stop"
CONF_AUTO_STOP_INACTIVITY_MS = "auto_stop_inactivity_ms"
# Per-device knobs. This component is shared byte-for-byte with the Voice PE repo (see
# voice-pe/home-assistant-voice-pe/shared_components.lock.json): express hardware differences here.
CONF_WEBSOCKET_BUFFER_SIZE = "websocket_buffer_size"
CONF_WAKE_OPEN_DELAY_MS = "wake_open_delay_ms"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(VoiceAssistantWebSocket),
        cv.Required(CONF_SERVER_URL): cv.string,
        cv.Optional(CONF_ENROLLMENT_WAKE_CHANNEL, default=1): cv.int_range(min=0, max=1),
        cv.Optional(CONF_ENROLLMENT_WAKE_GAIN, default=6): cv.int_range(min=1, max=64),
        # Model path or name; only the basename without extension is reported (e.g. penny_v5_oct1).
        cv.Optional(CONF_WAKE_TRIGGER_MODEL, default="unknown"): cv.string,
        cv.Optional(CONF_WAKE_TRIGGER_FW, default="unknown"): cv.string,
        cv.Optional(CONF_WAKE_TRIGGER_WINDOW, default=0): cv.int_range(min=0, max=255),
        cv.Optional(CONF_MICROPHONE): cv.use_id(microphone.Microphone),
        cv.Optional(CONF_SPEAKER): cv.use_id(speaker.Speaker),
        # Auto-stop the session after this long with no speaker (bot) audio.
        # Accepts a time period (e.g. "120s") or a raw millisecond integer.
        cv.Optional(
            CONF_AUTO_STOP_INACTIVITY_MS, default="120s"
        ): cv.positive_time_period_milliseconds,
        # esp_websocket_client RX/TX buffer. Larger frames are reassembled, so a small value only
        # costs more chunks. The Voice PE sets 1024 to save RAM while Sendspin owns the DMA pool.
        cv.Optional(CONF_WEBSOCKET_BUFFER_SIZE, default=4096): cv.int_range(min=512, max=16384),
        # Mic-gate delay used until the server's `hello` supplies the real value. 0 = no wake chime
        # (Satellite1); the Voice PE plays a chime and sets its delay.
        cv.Optional(CONF_WAKE_OPEN_DELAY_MS, default="0ms"): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_ON_CONNECTED): automation.validate_automation(single=True),
        cv.Optional(CONF_ON_DISCONNECTED): automation.validate_automation(single=True),
        cv.Optional(CONF_ON_ERROR): automation.validate_automation(single=True),
        cv.Optional(CONF_ON_STOPPED): automation.validate_automation(single=True),
        cv.Optional(CONF_ON_ENROLL_START): automation.validate_automation(single=True),
        cv.Optional(CONF_ON_ENROLL_STOP): automation.validate_automation(single=True),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    
    # Add ESP-IDF components. `CORE.using_esp_idf` was removed in newer ESPHome
    # (2026.x). Fall back to True since the Voice PE always builds with ESP-IDF.
    if getattr(CORE, "using_esp_idf", True):
        # WebSocket client component
        # Note: esp_websocket_client is a subdirectory in esp-protocols
        # We need to add the entire repository and reference the component path
        add_idf_component(
            name="esp-protocols",
            repo="https://github.com/espressif/esp-protocols.git",
            ref="websocket-v1.6.0",
            path="components/esp_websocket_client"
        )
    
    cg.add(var.set_server_url(config[CONF_SERVER_URL]))
    cg.add(var.set_enrollment_wake_channel(config[CONF_ENROLLMENT_WAKE_CHANNEL]))
    cg.add(var.set_enrollment_wake_gain_factor(config[CONF_ENROLLMENT_WAKE_GAIN]))
    cg.add(
        var.set_wake_trigger_meta(
            os.path.splitext(os.path.basename(config[CONF_WAKE_TRIGGER_MODEL]))[0],
            config[CONF_WAKE_TRIGGER_FW],
            config[CONF_WAKE_TRIGGER_WINDOW],
        )
    )
    cg.add(var.set_websocket_buffer_size(config[CONF_WEBSOCKET_BUFFER_SIZE]))
    cg.add(var.set_wake_open_delay_ms(config[CONF_WAKE_OPEN_DELAY_MS].total_milliseconds))
    cg.add(
        var.set_auto_stop_inactivity_ms(
            config[CONF_AUTO_STOP_INACTIVITY_MS].total_milliseconds
        )
    )

    if CONF_MICROPHONE in config:
        mic = await cg.get_variable(config[CONF_MICROPHONE])
        cg.add(var.set_microphone(mic))
    
    if CONF_SPEAKER in config:
        spkr = await cg.get_variable(config[CONF_SPEAKER])
        cg.add(var.set_speaker(spkr))
    
    # Register automation triggers
    if CONF_ON_CONNECTED in config:
        await automation.build_automation(
            var.get_connected_trigger(), [], config[CONF_ON_CONNECTED]
        )
    
    if CONF_ON_DISCONNECTED in config:
        await automation.build_automation(
            var.get_disconnected_trigger(), [], config[CONF_ON_DISCONNECTED]
        )
    
    if CONF_ON_ERROR in config:
        await automation.build_automation(
            var.get_error_trigger(), [], config[CONF_ON_ERROR]
        )
    
    if CONF_ON_STOPPED in config:
        await automation.build_automation(
            var.get_stopped_trigger(), [], config[CONF_ON_STOPPED]
        )

    if CONF_ON_ENROLL_START in config:
        await automation.build_automation(
            var.get_enroll_start_trigger(), [], config[CONF_ON_ENROLL_START]
        )

    if CONF_ON_ENROLL_STOP in config:
        await automation.build_automation(
            var.get_enroll_stop_trigger(), [], config[CONF_ON_ENROLL_STOP]
        )


# Register actions and conditions
VOICE_ASSISTANT_WEBSOCKET_ACTION_SCHEMA = maybe_simple_id(
    {
        cv.Required(CONF_ID): cv.use_id(VoiceAssistantWebSocket),
    }
)

VOICE_ASSISTANT_WEBSOCKET_CONDITION_SCHEMA = maybe_simple_id(
    {
        cv.Required(CONF_ID): cv.use_id(VoiceAssistantWebSocket),
    }
)


@automation.register_action(
    "voice_assistant_websocket.start",
    voice_assistant_websocket_ns.class_("VoiceAssistantWebSocketStartAction"),
    VOICE_ASSISTANT_WEBSOCKET_ACTION_SCHEMA,
)
async def voice_assistant_websocket_start_to_code(config, action_id, template_arg, args):
    paren = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, paren)


@automation.register_action(
    "voice_assistant_websocket.stop",
    voice_assistant_websocket_ns.class_("VoiceAssistantWebSocketStopAction"),
    VOICE_ASSISTANT_WEBSOCKET_ACTION_SCHEMA,
)
async def voice_assistant_websocket_stop_to_code(config, action_id, template_arg, args):
    paren = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, paren)


@automation.register_condition(
    "voice_assistant_websocket.is_running",
    voice_assistant_websocket_ns.class_("VoiceAssistantWebSocketIsRunningCondition"),
    VOICE_ASSISTANT_WEBSOCKET_CONDITION_SCHEMA,
)
async def voice_assistant_websocket_is_running_to_code(config, condition_id, template_arg, args):
    paren = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(condition_id, template_arg, paren)


@automation.register_condition(
    "voice_assistant_websocket.is_connected",
    voice_assistant_websocket_ns.class_("VoiceAssistantWebSocketIsConnectedCondition"),
    VOICE_ASSISTANT_WEBSOCKET_CONDITION_SCHEMA,
)
async def voice_assistant_websocket_is_connected_to_code(config, condition_id, template_arg, args):
    paren = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(condition_id, template_arg, paren)


@automation.register_action(
    "voice_assistant_websocket.interrupt",
    voice_assistant_websocket_ns.class_("VoiceAssistantWebSocketInterruptAction"),
    VOICE_ASSISTANT_WEBSOCKET_ACTION_SCHEMA,
)
async def voice_assistant_websocket_interrupt_to_code(config, action_id, template_arg, args):
    paren = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, paren)


@automation.register_condition(
    "voice_assistant_websocket.is_bot_speaking",
    voice_assistant_websocket_ns.class_("VoiceAssistantWebSocketIsBotSpeakingCondition"),
    VOICE_ASSISTANT_WEBSOCKET_CONDITION_SCHEMA,
)
async def voice_assistant_websocket_is_bot_speaking_to_code(config, condition_id, template_arg, args):
    paren = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(condition_id, template_arg, paren)


@automation.register_action(
    "voice_assistant_websocket.false_flag",
    voice_assistant_websocket_ns.class_("VoiceAssistantWebSocketFalseFlagAction"),
    VOICE_ASSISTANT_WEBSOCKET_ACTION_SCHEMA,
)
async def voice_assistant_websocket_false_flag_to_code(config, action_id, template_arg, args):
    paren = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, paren)


@automation.register_action(
    "voice_assistant_websocket.button_cancel",
    voice_assistant_websocket_ns.class_("VoiceAssistantWebSocketButtonCancelAction"),
    VOICE_ASSISTANT_WEBSOCKET_ACTION_SCHEMA,
)
async def voice_assistant_websocket_button_cancel_to_code(config, action_id, template_arg, args):
    paren = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, paren)


@automation.register_condition(
    "voice_assistant_websocket.turn_has_no_reply_audio",
    voice_assistant_websocket_ns.class_("VoiceAssistantWebSocketTurnHasNoReplyAudioCondition"),
    VOICE_ASSISTANT_WEBSOCKET_CONDITION_SCHEMA,
)
async def voice_assistant_websocket_turn_has_no_reply_audio_to_code(config, condition_id, template_arg, args):
    paren = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(condition_id, template_arg, paren)
