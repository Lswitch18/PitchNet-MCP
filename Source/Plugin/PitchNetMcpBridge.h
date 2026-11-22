#pragma once

#include "../JuceHeader.h"
#include <memory>

class PitchNetAudioProcessor;

/**
 * Lightweight Message-Thread IPC bridge allowing external MCP tools
 * (e.g., reaper-mcp) to inspect detected notes, set musical scale,
 * and apply per-note pitch/formant/gain edits directly inside PitchNet,
 * triggering PC-NSF-HiFiGAN neural resynthesis.
 *
 * Uses %TEMP%/pitchnet_mcp/{inst,cmd,resp}_<id>.json so no firewall
 * prompts or cross-thread audio locks are needed.
 */
class PitchNetMcpBridge : private juce::Timer
{
public:
    explicit PitchNetMcpBridge(PitchNetAudioProcessor& owner);
    ~PitchNetMcpBridge() override;

    juce::String getInstanceId() const { return instanceId; }

private:
    void timerCallback() override;
    void writeHeartbeat();
    void handleCommand(const juce::var& cmd);
    void writeResponse(const juce::var& payload);
    juce::var buildStateJson() const;

    PitchNetAudioProcessor& processor;
    juce::String instanceId;
    juce::File ipcDir;
    juce::File instFile;
    juce::File cmdFile;
    juce::File respFile;
    int heartbeatCounter = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PitchNetMcpBridge)
};
