#ifndef IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_DEFINE_MATH_OPERATORS
#endif
#include <imgui.h>
#include <imgui_internal.h>
#include <module.h>
#include <gui/gui.h>
#include <gui/style.h>
#include <signal_path/signal_path.h>
#include <core.h>
#include <config.h>
#include <json.hpp>
#include "signal_path/signal_path.h"
#include <radio_interface.h>
#include "if_nr.h"
#include "af_nr.h"
#include <gui/widgets/snr_meter.h>

ConfigManager config;

SDRPP_MOD_INFO{
    /* Name:        */ "noise_reduction_logmmse",
    /* Description: */ "LOGMMSE noise reduction",
    /* Author:      */ "sannysanoff",
    /* Version:     */ 0, 1, 0,
    /* Max instances */ -1
};

class NRModule : public ModuleManager::Instance {
    dsp::IFNRLogMMSE ifnrProcessor;
    std::unordered_map<std::string, std::shared_ptr<dsp::AFNRLogMMSE>>    afnrProcessors;   // logmmse por radio
    std::unordered_map<std::string, std::shared_ptr<dsp::AFNR_OMLSA_MCRA>> afnrProcessors2; // omlsa por radio

    // controla se ifnrProcessor ja foi inserido na preproc chain
    bool ifnrAdded = false;

public:
    NRModule(std::string name) {
        this->name = name;

        config.acquire();
        if (config.conf.contains("IFNR"))                  ifnr                  = config.conf["IFNR"];
        if (config.conf.contains("DisableCpuDeactivation")) disableCpuDeactivation = config.conf["DisableCpuDeactivation"];
        if (config.conf.contains("SNRChartWidget"))         snrChartWidget         = config.conf["SNRChartWidget"];
        config.release(true);

        ifnrProcessor.setDisableCpuDeactivation(disableCpuDeactivation);

        // SNR meter handler — registrado uma unica vez, para sempre
        ImGui::onSNRMeterExtPoint.bindHandler(&snrMeterExtPointHandler);
        snrMeterExtPointHandler.ctx = this;
        snrMeterExtPointHandler.handler = [](ImGui::SNRMeterExtPoint extp, void* ctx) {
            NRModule* _this = (NRModule*)ctx;
            if (!_this->enabled) return;
            _this->lastsnr.insert(_this->lastsnr.begin(), extp.lastDrawnValue);
            if (_this->lastsnr.size() > NLASTSNR) _this->lastsnr.resize(NLASTSNR);
            _this->postSnrLocation = extp.postSnrLocation;
        };

        gui::menu.registerEntry(name, menuHandler, this, NULL);

        // Insere o processador na chain UMA UNICA VEZ com enabled=true.
        // O bypass dentro de run() controla se processa ou faz passthrough.
        // Nunca chamamos removePreprocessor/addPreprocessor novamente para
        // evitar o crash de "block already part of chain" no chain.h.
        sigpath::iqFrontEnd.addPreprocessor(&ifnrProcessor, true);
        ifnrAdded = true;

        // Aplica o estado inicial do bypass baseado no config carregado
        actuateIFNR();

        // Registra os handlers de UI e radio (tune, radio instances)
        bindEventHandlers();
    }

    ~NRModule() {
        ImGui::onSNRMeterExtPoint.unbindHandler(&snrMeterExtPointHandler);
        gui::menu.removeEntry(name);
        unbindEventHandlers();
        if (ifnrAdded) {
            sigpath::iqFrontEnd.removePreprocessor(&ifnrProcessor);
        }
    }

    void postInit() {}

    void enable() {
        if (enabled) return;
        enabled = true;
        ifnrProcessor.setDisableCpuDeactivation(disableCpuDeactivation);
        actuateIFNR();       // aplica bypass conforme estado do checkbox
        bindEventHandlers(); // reconecta tune/radio handlers
    }

    void disable() {
        if (!enabled) return;
        enabled = false;
        // forcamos bypass=true quando o modulo inteiro esta desabilitado
        ifnrProcessor.bypass = true;
        unbindEventHandlers();
    }

    bool isEnabled() { return enabled; }

private:
    bool ifnr                  = false;
    bool disableCpuDeactivation = false;
    bool snrChartWidget        = false;
    bool handlersbound         = false;

    // -----------------------------------------------------------------------
    // Bind / unbind dos handlers de eventos (tune + radio instances)
    // Chamado apenas quando o modulo esta enabled.
    // -----------------------------------------------------------------------
    void bindEventHandlers() {
        if (handlersbound) return;
        handlersbound = true;

        gui::mainWindow.onWaterfallDrawn.bindHandler(&waterfallDrawnHandler);
        waterfallDrawnHandler.ctx = this;
        waterfallDrawnHandler.handler = [](ImGuiContext* gctx, void* ctx) {
            ((NRModule*)ctx)->drawSNRMeterAverages(gctx);
        };

        sigpath::sourceManager.onTuneChanged.bindHandler(&currentFrequencyChangedHandler);
        currentFrequencyChangedHandler.ctx = this;
        currentFrequencyChangedHandler.handler = [](double, void* ctx) {
            ((NRModule*)ctx)->ifnrProcessor.reset();
        };

        // anexa AF NR em todos os radios ja existentes
        auto names = core::modComManager.findInterfaces("radio");
        for (auto& n : names) { attachAFToRadio(n); }

        core::moduleManager.onInstanceCreated.bindHandler(&instanceCreatedHandler);
        instanceCreatedHandler.ctx = this;
        instanceCreatedHandler.handler = [](std::string v, void* ctx) {
            auto _this = (NRModule*)ctx;
            if (core::moduleManager.getInstanceModuleName(v) == "radio")
                _this->attachAFToRadio(v);
        };
    }

    void unbindEventHandlers() {
        if (!handlersbound) return;
        handlersbound = false;

        gui::mainWindow.onWaterfallDrawn.unbindHandler(&waterfallDrawnHandler);
        sigpath::sourceManager.onTuneChanged.unbindHandler(&currentFrequencyChangedHandler);
        core::moduleManager.onInstanceCreated.unbindHandler(&instanceCreatedHandler);

        // desanexa AF NR de todos os radios
        std::vector<std::string> toDetach;
        for (auto& [k, _] : afnrProcessors) toDetach.push_back(k);
        for (auto& k : toDetach) detachAFFromRadio(k);
    }

    // -----------------------------------------------------------------------
    // AF NR attachment
    // -----------------------------------------------------------------------
    void attachAFToRadio(const std::string& inst) {
        // evita duplo attach
        if (afnrProcessors.count(inst)) return;

        auto logmmse = std::make_shared<dsp::AFNRLogMMSE>();
        logmmse->init(nullptr);
        afnrProcessors[inst] = logmmse;

        auto omlsa = std::make_shared<dsp::AFNR_OMLSA_MCRA>();
        omlsa->init(nullptr);
        afnrProcessors2[inst] = omlsa;

        // logmmse vai para a IF chain do radio
        core::modComManager.callInterface(inst, RADIO_IFACE_CMD_ADD_TO_IFCHAIN,  logmmse.get(), NULL);
        // omlsa vai para a AF chain do radio
        core::modComManager.callInterface(inst, RADIO_IFACE_CMD_ADD_TO_AFCHAIN,  omlsa.get(), NULL);
        core::modComManager.callInterface(inst, RADIO_IFACE_CMD_ENABLE_IN_AFCHAIN, omlsa.get(), NULL);

        config.acquire();
        int  freq  = 10;    if (config.conf.contains("AF_NRF_" + inst))  freq  = config.conf["AF_NRF_" + inst];
        bool afnr  = false; if (config.conf.contains("AF_NR_"  + inst))  afnr  = config.conf["AF_NR_"  + inst];
        bool afnr2 = false; if (config.conf.contains("AF_NR2_" + inst))  afnr2 = config.conf["AF_NR2_" + inst];
        config.release(true);

        logmmse->afnrBandwidth = freq;
        logmmse->setProcessingBandwidth(freq * 1000);
        logmmse->allowed = afnr;
        omlsa->allowed   = afnr2;

        actuateAFNR();
    }

    void detachAFFromRadio(const std::string& inst) {
        auto it1 = afnrProcessors.find(inst);
        if (it1 != afnrProcessors.end()) {
            core::modComManager.callInterface(inst, RADIO_IFACE_CMD_REMOVE_FROM_IFCHAIN, it1->second.get(), NULL);
            afnrProcessors.erase(it1);
        }
        auto it2 = afnrProcessors2.find(inst);
        if (it2 != afnrProcessors2.end()) {
            core::modComManager.callInterface(inst, RADIO_IFACE_CMD_REMOVE_FROM_AFCHAIN, it2->second.get(), NULL);
            afnrProcessors2.erase(it2);
        }
    }

    // -----------------------------------------------------------------------
    // Actuation
    // -----------------------------------------------------------------------

    // Baseband NR: apenas altera o flag bypass — o processador permanece
    // sempre ativo na chain para evitar o bug de blockBefore() no chain.h
    void actuateIFNR() {
        ifnrProcessor.bypass = !(enabled && ifnr);
    }

    // AF NR logmmse: enable/disable na IF chain do radio
    void actuateAFNR() {
        for (auto& [k, v] : afnrProcessors) {
            int cmd = v->allowed ? RADIO_IFACE_CMD_ENABLE_IN_IFCHAIN
                                 : RADIO_IFACE_CMD_DISABLE_IN_IFCHAIN;
            core::modComManager.callInterface(k, cmd, v.get(), NULL);
        }
        // OMLSA (afnrProcessors2) e controlado pelo campo allowed lido
        // diretamente pelo processador; nao precisa de cmd separado.
    }

    // -----------------------------------------------------------------------
    // Menu
    // -----------------------------------------------------------------------
    void menuHandler() {
        // --- Baseband NR ---
        if (ImGui::Checkbox("Baseband NR##_sdrpp_if_nr", &ifnr)) {
            config.acquire();
            config.conf["IFNR"] = ifnr;
            config.release(true);
            if (ifnr) ifnrProcessor.stopReason = "";
            actuateIFNR();
        }
        ImGui::SameLine();

        // se o processador pediu parada por CPU, reflete no checkbox
        if (ifnrProcessor.stopReason != "" && ifnr) {
            ifnr = false;
            config.acquire(); config.conf["IFNR"] = ifnr; config.release(true);
            actuateIFNR();
        }

        if (ifnrProcessor.stopReason != "") {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1,0,0,1));
            ImGui::Text("%s", ifnrProcessor.stopReason.c_str());
            ImGui::PopStyleColor();
        } else if (ifnrProcessor.percentUsage >= 0) {
            if (ifnrProcessor.percentUsage > 80) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1,0,0,1));
            std::string cpu = std::to_string((int)ifnrProcessor.percentUsage) + "% cpu";
            ImVec2 sz = ImGui::CalcTextSize(cpu.c_str());
            if (ImGui::Selectable(cpu.c_str(), false, ImGuiSelectableFlags_None, sz)) {
                disableCpuDeactivation = !disableCpuDeactivation;
                ifnrProcessor.setDisableCpuDeactivation(disableCpuDeactivation);
                config.acquire(); config.conf["DisableCpuDeactivation"] = disableCpuDeactivation; config.release(true);
            }
            if (disableCpuDeactivation) {
                auto dl = ImGui::GetWindowDrawList();
                ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
                dl->AddLine({mn.x,(mn.y+mx.y)/2}, {mx.x,(mn.y+mx.y)/2}, ImGui::GetColorU32(ImGuiCol_Text));
            }
            if (ifnrProcessor.percentUsage > 80) ImGui::PopStyleColor();
        }

        // --- Audio NR (logmmse) por radio ---
        for (auto& [k, v] : afnrProcessors) {
            if (ImGui::Checkbox(("Audio NR " + k + "##_af_logmmse_" + k).c_str(), &v->allowed)) {
                actuateAFNR();
                config.acquire(); config.conf["AF_NR_" + k] = v->allowed; config.release(true);
            }
        }

        // --- Audio NR2 (OMLSA) por radio ---
        for (auto& [k, v] : afnrProcessors2) {
            if (ImGui::Checkbox(("Audio NR2 " + k + "##_af_omlsa_" + k).c_str(), &v->allowed)) {
                config.acquire(); config.conf["AF_NR2_" + k] = v->allowed; config.release(true);
            }
            ImGui::SameLine();
            ImGui::Text("%0.01f", 32767.0 / v->scaled);
        }

        // --- SNR Chart ---
        if (ImGui::Checkbox(("SNR Chart##_snr_" + name).c_str(), &snrChartWidget)) {
            config.acquire(); config.conf["SNRChartWidget"] = snrChartWidget; config.release(true);
        }
    }

    static void menuHandler(void* ctx) { ((NRModule*)ctx)->menuHandler(); }

    // -----------------------------------------------------------------------
    // SNR chart overlay
    // -----------------------------------------------------------------------
    static const int NLASTSNR = 1500;
    std::vector<float> lastsnr;
    ImVec2 postSnrLocation;

    void drawSNRMeterAverages(ImGuiContext* gctx) {
        if (!snrChartWidget || !enabled) return;
        static std::vector<float> r;
        static int counter = 0;
        static const int winsize = 10;
        if (++counter % winsize == winsize - 1)
            r = dsp::math::maxeach(winsize, lastsnr);
        ImGuiWindow* w = gctx->CurrentWindow;
        ImU32 col = ImGui::GetColorU32(ImGuiCol_Text);
        for (int q = 1; q < (int)r.size(); q++)
            w->DrawList->AddLine(
                postSnrLocation + ImVec2(r[q-1], (float)(q-1) + w->Pos.y),
                postSnrLocation + ImVec2(r[q],   (float)q     + w->Pos.y), col);
    }

    // -----------------------------------------------------------------------
    // Members
    // -----------------------------------------------------------------------
    std::string name;
    bool enabled = true;
    EventHandler<ImGuiContext*>          waterfallDrawnHandler;
    EventHandler<ImGui::SNRMeterExtPoint> snrMeterExtPointHandler;
    EventHandler<double>                 currentFrequencyChangedHandler;
    EventHandler<std::string>            instanceCreatedHandler;
};

MOD_EXPORT void _INIT_() {
    config.setPath(std::string(core::getRoot()) + "/noise_reduction_logmmse_config.json");
    config.load(json::object());
    config.enableAutoSave();
}
MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) { return new NRModule(name); }
MOD_EXPORT void _DELETE_INSTANCE_(void* instance) { delete (NRModule*)instance; }
MOD_EXPORT void _END_() { config.disableAutoSave(); config.save(); }
