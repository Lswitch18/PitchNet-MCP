#include "PitchNetMcpBridge.h"
#include "PluginProcessor.h"
#include "../Models/Project.h"

PitchNetMcpBridge::PitchNetMcpBridge(PitchNetAudioProcessor& owner)
    : processor(owner)
{
    instanceId = "pn_" + juce::String::toHexString(reinterpret_cast<juce::int64>(&owner));
    ipcDir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("pitchnet_mcp");
    ipcDir.createDirectory();

    instFile = ipcDir.getChildFile("inst_" + instanceId + ".json");
    cmdFile  = ipcDir.getChildFile("cmd_" + instanceId + ".json");
    respFile = ipcDir.getChildFile("resp_" + instanceId + ".json");

    writeHeartbeat();
    startTimer(150);
}

PitchNetMcpBridge::~PitchNetMcpBridge()
{
    stopTimer();
    instFile.deleteFile();
    cmdFile.deleteFile();
    respFile.deleteFile();
}

void PitchNetMcpBridge::timerCallback()
{
    if (++heartbeatCounter >= 10)
    {
        heartbeatCounter = 0;
        writeHeartbeat();
    }

    if (cmdFile.existsAsFile())
    {
        auto text = cmdFile.loadFileAsString();
        cmdFile.deleteFile();
        auto parsed = juce::JSON::parse(text);
        if (parsed.isObject())
            handleCommand(parsed);
    }
}

void PitchNetMcpBridge::writeHeartbeat()
{
    auto* obj = new juce::DynamicObject();
    obj->setProperty("instanceId", instanceId);
    obj->setProperty("pid", static_cast<int>(juce::Process::getCurrentProcessId()));
    obj->setProperty("araActive", processor.isARAModeActive());
    obj->setProperty("sampleRate", processor.getHostSampleRate());

    const Project* proj = processor.projectForPersistentState();
    obj->setProperty("hasProject", proj != nullptr);
    obj->setProperty("noteCount", proj != nullptr ? static_cast<int>(proj->getNotes().size()) : 0);
    obj->setProperty("updatedAtMs", static_cast<juce::int64>(juce::Time::currentTimeMillis()));

    instFile.replaceWithText(juce::JSON::toString(juce::var(obj)));
}

void PitchNetMcpBridge::writeResponse(const juce::var& payload)
{
    respFile.replaceWithText(juce::JSON::toString(payload));
}

juce::var PitchNetMcpBridge::buildStateJson() const
{
    auto* root = new juce::DynamicObject();
    root->setProperty("instanceId", instanceId);
    root->setProperty("araActive", processor.isARAModeActive());

    const Project* proj = processor.projectForPersistentState();
    if (proj == nullptr)
    {
        root->setProperty("hasProject", false);
        root->setProperty("notes", juce::Array<juce::var>());
        return juce::var(root);
    }

    root->setProperty("hasProject", true);
    root->setProperty("sampleRate", proj->getAudioData().sampleRate);
    root->setProperty("timelineOffsetSeconds", proj->getAudioData().timelineOffsetSeconds);
    root->setProperty("pitchCenter", proj->getPitchCenter());
    root->setProperty("globalPitchOffset", proj->getGlobalPitchOffset());
    root->setProperty("formantShift", proj->getFormantShift());
    root->setProperty("scaleRoot", proj->getProjectKeyRoot());
    root->setProperty("scaleMode", static_cast<int>(proj->getProjectKeyMode()));

    juce::Array<juce::var> notesArr;
    const auto& notes = proj->getNotes();
    for (int i = 0; i < static_cast<int>(notes.size()); ++i)
    {
        const auto& n = notes[static_cast<size_t>(i)];
        auto* nObj = new juce::DynamicObject();
        nObj->setProperty("index", i);
        nObj->setProperty("startFrame", n.getStartFrame());
        nObj->setProperty("endFrame", n.getEndFrame());
        nObj->setProperty("midiNote", n.getMidiNote());
        nObj->setProperty("originalMidiNote", n.getOriginalMidiNote());
        nObj->setProperty("pitchOffset", n.getPitchOffset());
        nObj->setProperty("formantShift", n.getFormantShift());
        nObj->setProperty("volumeDb", n.getVolumeDb());
        nObj->setProperty("macroPitchCenter", n.getMacroPitchCenter());
        notesArr.add(juce::var(nObj));
    }
    root->setProperty("notes", notesArr);
    return juce::var(root);
}

void PitchNetMcpBridge::handleCommand(const juce::var& cmd)
{
    auto action = cmd.getProperty("action", "").toString();

    if (action == "get_state")
    {
        auto* resp = new juce::DynamicObject();
        resp->setProperty("ok", true);
        resp->setProperty("state", buildStateJson());
        writeResponse(juce::var(resp));
        return;
    }

    const Project* proj = processor.projectForPersistentState();
    if (proj == nullptr)
    {
        auto* resp = new juce::DynamicObject();
        resp->setProperty("ok", false);
        resp->setProperty("error", "No active PitchNet project/captured audio yet. Capture or bind ARA first.");
        writeResponse(juce::var(resp));
        return;
    }

    if (action == "set_scale")
    {
        Project updated(*proj);
        int rootVal = cmd.hasProperty("root") ? static_cast<int>(cmd.getProperty("root", 0)) : updated.getProjectKeyRoot();
        ScaleMode modeVal = cmd.hasProperty("mode") ? static_cast<ScaleMode>(static_cast<int>(cmd.getProperty("mode", 1))) : updated.getProjectKeyMode();
        updated.setProjectKey(rootVal, modeVal);
        processor.updateProjectStateFromEditor(updated);

        auto* resp = new juce::DynamicObject();
        resp->setProperty("ok", true);
        resp->setProperty("state", buildStateJson());
        writeResponse(juce::var(resp));
        return;
    }

    if (action == "edit_notes")
    {
        Project updated(*proj);
        if (cmd.hasProperty("pitchCenter"))
            updated.setPitchCenter(static_cast<float>(cmd.getProperty("pitchCenter", 0.0)));

        auto edits = cmd.getProperty("edits", juce::var());
        int applied = 0;
        if (edits.isArray())
        {
            auto& notes = updated.getNotes();
            for (const auto& item : *edits.getArray())
            {
                if (!item.isObject()) continue;
                int idx = static_cast<int>(item.getProperty("index", -1));
                if (idx < 0 || idx >= static_cast<int>(notes.size())) continue;

                auto& n = notes[static_cast<size_t>(idx)];
                if (item.hasProperty("midiNote"))
                    n.setMidiNote(static_cast<float>(item.getProperty("midiNote", n.getMidiNote())));
                if (item.hasProperty("pitchOffset"))
                    n.setPitchOffset(static_cast<float>(item.getProperty("pitchOffset", n.getPitchOffset())));
                if (item.hasProperty("formantShift"))
                    n.setFormantShift(static_cast<float>(item.getProperty("formantShift", n.getFormantShift())));
                if (item.hasProperty("volumeDb"))
                    n.setVolumeDb(static_cast<float>(item.getProperty("volumeDb", n.getVolumeDb())));
                updated.markNoteEditDirtyRange(n.getStartFrame(), n.getEndFrame());
                ++applied;
            }
        }

        processor.updateProjectStateFromEditor(updated);
        processor.requestPluginProjectRender(updated);
        writeHeartbeat();

        auto* resp = new juce::DynamicObject();
        resp->setProperty("ok", true);
        resp->setProperty("applied", applied);
        writeResponse(juce::var(resp));
        return;
    }

    auto* resp = new juce::DynamicObject();
    resp->setProperty("ok", false);
    resp->setProperty("error", "Unknown action: " + action);
    writeResponse(juce::var(resp));
}
