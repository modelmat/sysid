// Copyright (c) FIRST and other WPILib contributors.
// Open Source Software; you can modify and/or share it under the terms of
// the WPILib BSD license file in the root directory of this project.

#include "sysid/view/AnalyzerPlot.h"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <thread>
#include <vector>

#include <fmt/format.h>
#include <imgui.h>
#include <implot.h>
#include <units/math.h>
#include <units/time.h>

#include "sysid/Util.h"
#include "sysid/analysis/AnalysisManager.h"
#include "sysid/analysis/AnalysisType.h"
#include "sysid/analysis/ArmSim.h"
#include "sysid/analysis/ElevatorSim.h"
#include "sysid/analysis/FilteringUtils.h"
#include "sysid/analysis/SimpleMotorSim.h"

using namespace sysid;

static ImPlotPoint Getter(void* data, int idx) {
  return static_cast<ImPlotPoint*>(data)[idx];
}

static double simSquaredErrorSum = 0.0;
static double squaredVariationSum = 0.0;
static int timeSeriesPoints = 0;

template <typename Model>
static std::vector<std::vector<ImPlotPoint>> PopulateTimeDomainSim(
    const std::vector<PreparedData>& data,
    const std::array<units::second_t, 4>& startTimes, size_t step,
    Model model) {
  // Create the vector of ImPlotPoints that will contain our simulated data.
  std::vector<std::vector<ImPlotPoint>> pts;
  std::vector<ImPlotPoint> tmp;

  auto startTime = data[0].timestamp;

  tmp.emplace_back(startTime.value(), data[0].velocity);

  model.Reset(data[0].position, data[0].velocity);
  units::second_t t = 0_s;

  for (size_t i = 1; i < data.size(); ++i) {
    const auto& now = data[i];
    const auto& pre = data[i - 1];

    t += now.timestamp - pre.timestamp;

    // If the current time stamp and previous time stamp are across a test's
    // start timestamp, it is the start of a new test and the model needs to be
    // reset.
    if (std::find(startTimes.begin(), startTimes.end(), now.timestamp) !=
        startTimes.end()) {
      pts.emplace_back(std::move(tmp));
      model.Reset(now.position, now.velocity);
      continue;
    }

    model.Update(units::volt_t{pre.voltage}, pre.dt);
    tmp.emplace_back((startTime + t).value(), model.GetVelocity());
    simSquaredErrorSum += std::pow(now.velocity - model.GetVelocity(), 2);
    squaredVariationSum += std::pow(now.velocity, 2);
    timeSeriesPoints++;
  }

  pts.emplace_back(std::move(tmp));
  return pts;
}

AnalyzerPlot::AnalyzerPlot(wpi::Logger& logger) : m_logger(logger) {
  // Pre-allocate our vectors with the max data size.
  for (auto&& title : kChartTitles) {
    m_filteredData[title].reserve(kMaxSize);
  }
}

void AnalyzerPlot::ResetData() {
  // Clear all data vectors.
  for (auto it = m_filteredData.begin(); it != m_filteredData.end(); ++it) {
    it->second.clear();
  }

  for (auto it = m_rawData.begin(); it != m_rawData.end(); ++it) {
    it->second.clear();
  }

  m_dtMeanLine.clear();
  m_quasistaticSim.clear();
  m_dynamicSim.clear();
  simSquaredErrorSum = 0;
  squaredVariationSum = 0;
  timeSeriesPoints = 0;

  // Reset Lines of Best Fit
  m_KvFit[0] = ImPlotPoint(0, 0);
  m_KvFit[1] = ImPlotPoint(0, 0);
  m_KaFit[0] = ImPlotPoint(0, 0);
  m_KaFit[1] = ImPlotPoint(0, 0);

  // Fit Plots
  FitPlots();
}

void AnalyzerPlot::SetRawTimeData(const std::vector<PreparedData>& rawSlow,
                                  const std::vector<PreparedData>& rawFast,
                                  std::atomic<bool>& abort) {
  auto rawSlowStep = std::ceil(rawSlow.size() * 1.0 / kMaxSize * 4);
  auto rawFastStep = std::ceil(rawFast.size() * 1.0 / kMaxSize * 4);
  // Populate Raw Slow Time Series Data
  for (size_t i = 0; i < rawSlow.size(); i += rawSlowStep) {
    if (abort) {
      return;
    }
    m_rawData[kChartTitles[2]].emplace_back((rawSlow[i].timestamp).value(),
                                            rawSlow[i].velocity);
    m_rawData[kChartTitles[3]].emplace_back((rawSlow[i].timestamp).value(),
                                            rawSlow[i].acceleration);
  }

  // Populate Raw fast Time Series Data
  for (size_t i = 0; i < rawFast.size(); i += rawFastStep) {
    if (abort) {
      return;
    }
    m_rawData[kChartTitles[4]].emplace_back((rawFast[i].timestamp).value(),
                                            rawFast[i].velocity);
    m_rawData[kChartTitles[5]].emplace_back((rawFast[i].timestamp).value(),
                                            rawFast[i].acceleration);
  }
}

void AnalyzerPlot::SetGraphLabels(std::string_view unit) {
  auto abbreviation = GetAbbreviation(unit);
  m_velocityLabel = fmt::format("Velocity ({} / s)", abbreviation);
  m_accelerationLabel = fmt::format("Acceleration ({} / s^2)", abbreviation);
}

void AnalyzerPlot::SetRawData(const Storage& rawData, std::string_view unit,
                              std::atomic<bool>& abort) {
  auto& [rawSlow, rawFast] = rawData;
  SetGraphLabels(unit);

  std::scoped_lock lock(m_mutex);

  ResetData();
  SetRawTimeData(rawSlow, rawFast, abort);
}

void AnalyzerPlot::SetData(const Storage& rawData, const Storage& filteredData,
                           std::string_view unit,
                           const std::vector<double>& ffGains,
                           const std::array<units::second_t, 4>& startTimes,
                           AnalysisType type, std::atomic<bool>& abort) {
  auto& [slow, fast] = filteredData;
  auto& [rawSlow, rawFast] = rawData;
  const auto& Ks = ffGains[0];
  const auto& Kv = ffGains[1];
  const auto& Ka = ffGains[2];

  SetGraphLabels(unit);

  std::scoped_lock lock(m_mutex);

  ResetData();

  // Calculate step sizes to ensure that we only use the memory that we
  // allocated.
  auto slowStep = std::ceil(slow.size() * 1.0 / kMaxSize * 4);
  auto fastStep = std::ceil(fast.size() * 1.0 / kMaxSize * 4);

  // Calculate min and max velocities and accelerations of the slow and fast
  // datasets respectively.
  auto slowMinElement =
      std::min_element(slow.cbegin(), slow.cend(), [](auto& a, auto& b) {
        return a.velocity < b.velocity;
      })->velocity;

  auto slowMaxElement =
      std::max_element(slow.cbegin(), slow.cend(), [](auto& a, auto& b) {
        return a.velocity < b.velocity;
      })->velocity;

  auto fastMinElement =
      std::min_element(fast.cbegin(), fast.cend(), [](auto& a, auto& b) {
        return a.acceleration < b.acceleration;
      })->acceleration;

  auto fastMaxElement =
      std::max_element(fast.cbegin(), fast.cend(), [](auto& a, auto& b) {
        return a.acceleration < b.acceleration;
      })->acceleration;

  units::second_t dtMean = GetMeanTimeDelta(filteredData);

  // Populate quasistatic time-domain graphs and quasistatic velocity vs.
  // velocity-portion voltage graph.

  for (size_t i = 0; i < slow.size(); i += slowStep) {
    if (abort) {
      return;
    }
    // Calculate portion of voltage that corresponds to change in velocity.
    double Vportion = slow[i].voltage - std::copysign(Ks, slow[i].velocity) -
                      Ka * slow[i].acceleration;

    if (type == analysis::kElevator) {
      const auto& Kg = ffGains[3];
      Vportion -= Kg;
    } else if (type == analysis::kArm) {
      const auto& Kcos = ffGains[3];
      Vportion -= Kcos * slow[i].cos;
    }

    // Calculate points to show the line of best fit.
    m_KvFit[0] = ImPlotPoint(Kv * slowMinElement, slowMinElement);
    m_KvFit[1] = ImPlotPoint(Kv * slowMaxElement, slowMaxElement);

    m_filteredData[kChartTitles[0]].emplace_back(Vportion, slow[i].velocity);
    m_filteredData[kChartTitles[2]].emplace_back((slow[i].timestamp).value(),
                                                 slow[i].velocity);
    m_filteredData[kChartTitles[3]].emplace_back((slow[i].timestamp).value(),
                                                 slow[i].acceleration);

    if (i > 0) {
      // If the current timestamp is not in the startTimes array, it is the
      // during a test and should be included. If it is in the startTimes array,
      // it is the beginning of a new test and the dt will be inflated.
      // Therefore we skip those to exclude that dt and effectively reset dt
      // calculations.
      if (slow[i].dt > 0_s &&
          std::find(startTimes.begin(), startTimes.end(), slow[i].timestamp) ==
              startTimes.end()) {
        m_filteredData[kChartTitles[6]].emplace_back(
            (slow[i].timestamp).value(),
            units::millisecond_t{slow[i].dt}.value());
      }
    }
  }

  // Populate dynamic time-domain graphs and dynamic acceleration vs.
  // acceleration-portion voltage graph.
  for (size_t i = 0; i < fast.size(); i += fastStep) {
    if (abort) {
      return;
    }
    // Calculate portion of voltage that corresponds to change in acceleration.
    double Vportion = fast[i].voltage - std::copysign(Ks, fast[i].velocity) -
                      Kv * fast[i].velocity;

    if (type == analysis::kElevator) {
      const auto& Kg = ffGains[3];
      Vportion -= Kg;
    } else if (type == analysis::kArm) {
      const auto& Kcos = ffGains[3];
      Vportion -= Kcos * fast[i].cos;
    }

    // Calculate points to show the line of best fit.
    m_KaFit[0] = ImPlotPoint(Ka * fastMinElement, fastMinElement);
    m_KaFit[1] = ImPlotPoint(Ka * fastMaxElement, fastMaxElement);

    m_filteredData[kChartTitles[1]].emplace_back(Vportion,
                                                 fast[i].acceleration);
    m_filteredData[kChartTitles[4]].emplace_back((fast[i].timestamp).value(),
                                                 fast[i].velocity);
    m_filteredData[kChartTitles[5]].emplace_back((fast[i].timestamp).value(),
                                                 fast[i].acceleration);
    if (i > 0) {
      // If the current timestamp is not in the startTimes array, it is the
      // during a test and should be included. If it is in the startTimes array,
      // it is the beginning of a new test and the dt will be inflated.
      // Therefore we skip those to exclude that dt and effectively reset dt
      // calculations.
      if (fast[i].dt > 0_s &&
          std::find(startTimes.begin(), startTimes.end(), fast[i].timestamp) ==
              startTimes.end()) {
        m_filteredData[kChartTitles[6]].emplace_back(
            (fast[i].timestamp).value(),
            units::millisecond_t{fast[i].dt}.value());
      }
    }
  }

  auto minTime =
      units::math::min(slow.front().timestamp, fast.front().timestamp);
  auto maxTime = units::math::max(slow.back().timestamp, fast.back().timestamp);

  // Set first recorded timestamp to mean
  m_dtMeanLine.emplace_back(minTime.value(),
                            units::millisecond_t{dtMean}.value());

  // Set last recorded timestamp to mean
  m_dtMeanLine.emplace_back(maxTime.value(),
                            units::millisecond_t{dtMean}.value());

  SetRawTimeData(rawSlow, rawFast, abort);

  // Populate Simulated Time Series Data.
  if (type == analysis::kElevator) {
    const auto& Kg = ffGains[3];
    m_quasistaticSim = PopulateTimeDomainSim(
        rawSlow, startTimes, fastStep, sysid::ElevatorSim{Ks, Kv, Ka, Kg});
    m_dynamicSim = PopulateTimeDomainSim(rawFast, startTimes, fastStep,
                                         sysid::ElevatorSim{Ks, Kv, Ka, Kg});
  } else if (type == analysis::kArm) {
    const auto& Kcos = ffGains[3];
    m_quasistaticSim = PopulateTimeDomainSim(rawSlow, startTimes, fastStep,
                                             sysid::ArmSim{Ks, Kv, Ka, Kcos});
    m_dynamicSim = PopulateTimeDomainSim(rawFast, startTimes, fastStep,
                                         sysid::ArmSim{Ks, Kv, Ka, Kcos});
  } else {
    m_quasistaticSim = PopulateTimeDomainSim(rawSlow, startTimes, fastStep,
                                             sysid::SimpleMotorSim{Ks, Kv, Ka});
    m_dynamicSim = PopulateTimeDomainSim(rawFast, startTimes, fastStep,
                                         sysid::SimpleMotorSim{Ks, Kv, Ka});
  }

  // RMSE = std::sqrt(sum((x_i - x^_i)^2) / N) where sum represents the sum of
  // all time series points, x_i represents the velocity at a timestep, x^_i
  // represents the prediction at the timestep, and N represents the number of
  // points
  m_RMSE = std::sqrt(simSquaredErrorSum / timeSeriesPoints);
  m_RSquared = 1 - m_RMSE / std::sqrt(squaredVariationSum / timeSeriesPoints);
  FitPlots();
}

void AnalyzerPlot::FitPlots() {
  // Set the "fit" flag to true.
  for (auto& f : m_fitNextPlot) {
    f = true;
  }
}

bool AnalyzerPlot::DisplayVoltageDomainPlots(ImVec2 plotSize) {
  std::unique_lock lock(m_mutex, std::defer_lock);

  if (!lock.try_lock()) {
    ImGui::Text("Loading %c",
                "|/-\\"[static_cast<int>(ImGui::GetTime() / 0.05f) & 3]);
    return false;
  }

  bool forPicture = plotSize.x != -1;

  // Quasistatic Velocity vs. Velocity Portion Voltage.
  if (m_fitNextPlot[0]) {
    ImPlot::SetNextAxesToFit();
  }

  if (ImPlot::BeginPlot(kChartTitles[0], plotSize, ImPlotFlags_None)) {
    ImPlot::SetupAxis(ImAxis_X1, "Velocity-Portion Voltage",
                      ImPlotAxisFlags_NoGridLines);
    ImPlot::SetupAxis(ImAxis_Y1, "Quasistatic Velocity",
                      ImPlotAxisFlags_NoGridLines);
    // Get a reference to the data that we are plotting.
    auto& data = m_filteredData[kChartTitles[0]];

    ImPlot::SetNextMarkerStyle(IMPLOT_AUTO, 1, IMPLOT_AUTO_COL, 0);
    ImPlot::PlotScatterG("Filtered Data", Getter, data.data(), data.size());

    ImPlot::SetNextLineStyle(IMPLOT_AUTO_COL, 1.5);
    ImPlot::PlotLineG("Fit", Getter, m_KvFit, 2);

    ImPlot::EndPlot();

    if (m_fitNextPlot[0]) {
      m_fitNextPlot[0] = false;
    }
  }

  if (m_fitNextPlot[1]) {
    ImPlot::SetNextAxesToFit();
  }

  if (forPicture) {
    ImGui::SameLine();
  }

  if (ImPlot::BeginPlot(kChartTitles[1], plotSize, ImPlotFlags_None)) {
    ImPlot::SetupAxis(ImAxis_X1, "Acceleration-Portion Voltage",
                      ImPlotAxisFlags_NoGridLines);
    ImPlot::SetupAxis(ImAxis_Y1, "Dynamic Acceleration",
                      ImPlotAxisFlags_NoGridLines);
    // Get a reference to the data we are plotting.
    auto& data = m_filteredData[kChartTitles[1]];

    ImPlot::SetNextMarkerStyle(IMPLOT_AUTO, 1, IMPLOT_AUTO_COL, 0);
    ImPlot::PlotScatterG("Filtered Data", Getter, data.data(), data.size());

    ImPlot::SetNextLineStyle(IMPLOT_AUTO_COL, 1.5);
    ImPlot::PlotLineG("Fit", Getter, m_KaFit, 2);

    ImPlot::EndPlot();

    if (m_fitNextPlot[1]) {
      m_fitNextPlot[1] = false;
    }
  }
  return true;
}

static void PlotSimData(std::vector<std::vector<ImPlotPoint>>& data) {
  for (auto&& pts : data) {
    ImPlot::SetNextLineStyle(IMPLOT_AUTO_COL, 1.5);
    ImPlot::PlotLineG("Simulation", Getter, pts.data(), pts.size());
  }
}

static void PlotRawAndFiltered(std::vector<ImPlotPoint>& rawData,
                               std::vector<ImPlotPoint>& filteredData) {
  ImPlot::SetNextMarkerStyle(IMPLOT_AUTO, 1, IMPLOT_AUTO_COL, 0);
  ImPlot::PlotScatterG("Raw Data", Getter, rawData.data(), rawData.size());
  // Plot Filtered Data after Raw data
  ImPlot::SetNextMarkerStyle(IMPLOT_AUTO, 1, IMPLOT_AUTO_COL, 0);
  ImPlot::PlotScatterG("Filtered Data", Getter, filteredData.data(),
                       filteredData.size());
}

bool AnalyzerPlot::DisplayTimeDomainPlots(ImVec2 plotSize) {
  std::unique_lock lock(m_mutex, std::defer_lock);

  if (!lock.try_lock()) {
    ImGui::Text("Loading %c",
                "|/-\\"[static_cast<int>(ImGui::GetTime() / 0.05f) & 3]);
    return false;
  }

  bool forPicture = plotSize.x != -1;

  // Iterate through the chart titles for these plots and graph them.
  for (size_t i = 2; i < 6; ++i) {
    const char* x = "Time (s)";
    const char* y =
        i % 2 == 0 ? m_velocityLabel.c_str() : m_accelerationLabel.c_str();
    bool isVelocity = (i == 2 || i == 4);

    // Get a reference to the data we are plotting.
    auto& filteredData = m_filteredData[kChartTitles[i]];
    auto& rawData = m_rawData[kChartTitles[i]];

    // Generate Sim vs Filtered Plot
    if (m_fitNextPlot[i]) {
      ImPlot::SetNextAxesToFit();
    }

    if (forPicture && i % 4 != 0) {
      ImGui::SameLine();
    }

    if (ImPlot::BeginPlot(kChartTitles[i], plotSize, ImPlotFlags_None)) {
      ImPlot::SetupAxis(ImAxis_X1, x, ImPlotAxisFlags_NoGridLines);
      ImPlot::SetupAxis(ImAxis_Y1, y, ImPlotAxisFlags_NoGridLines);
      // Set Legend Location:
      ImPlot::SetupLegend(ImPlotLocation_NorthEast);

      // Plot Raw and Filtered Data
      PlotRawAndFiltered(rawData, filteredData);

      // Plot Simulation Data for Velocity Data
      if (isVelocity) {
        PlotSimData((i == 2) ? m_quasistaticSim : m_dynamicSim);
      }

      // Disable constant resizing for Accel Plot
      if (m_fitNextPlot[i]) {
        m_fitNextPlot[i] = false;
      }

      ImPlot::EndPlot();
    }
  }

  if (forPicture) {
    ImGui::SameLine();
  }

  if (m_fitNextPlot[6]) {
    ImPlot::SetNextAxisToFit(ImAxis_X1);
  }

  if (ImPlot::BeginPlot(kChartTitles[6], plotSize, ImPlotFlags_None)) {
    ImPlot::SetupAxisLimits(ImAxis_Y1, 0, 50);
    ImPlot::SetupAxis(ImAxis_X1, "Time (s)", ImPlotAxisFlags_NoGridLines);
    ImPlot::SetupAxis(ImAxis_Y1, "Change in Time (ms)",
                      ImPlotAxisFlags_NoGridLines);
    // Get a reference to the data we are plotting.
    auto& data = m_filteredData[kChartTitles[6]];

    ImPlot::SetNextMarkerStyle(IMPLOT_AUTO, 1, IMPLOT_AUTO_COL, 0);
    ImPlot::PlotScatterG("Timesteps", Getter, data.data(), data.size());

    ImPlot::SetNextMarkerStyle(IMPLOT_AUTO, 1, IMPLOT_AUTO_COL, 0);

    ImPlot::PlotLineG("Mean dt", Getter, m_dtMeanLine.data(),
                      m_dtMeanLine.size());

    ImPlot::EndPlot();

    if (m_fitNextPlot[6]) {
      m_fitNextPlot[6] = false;
    }
  }
  return true;
}

bool AnalyzerPlot::LoadPlots() {
  // See if the plots are loaded
  return DisplayTimeDomainPlots() && DisplayVoltageDomainPlots();
}

void AnalyzerPlot::DisplayCombinedPlots() {
  const ImVec2 plotSize{kCombinedPlotSize, kCombinedPlotSize};
  DisplayVoltageDomainPlots(plotSize);
  DisplayTimeDomainPlots(plotSize);
}
