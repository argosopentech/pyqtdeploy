// Copyright (c) 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

const CalculatorModifier = Object.freeze({
  kNone: Object.freeze({postfix: '', multiplier: 1}),
  kMillisecondsFromSeconds:
      Object.freeze({postfix: '_in_ms', multiplier: 1000}),
});

class Metric {
  constructor(name, value) {
    this.name = name;
    this.value = value;
  }

  toString() {
    return '{"' + this.name + '":"' + this.value + '"}';
  }
}

// Represents a companion dictionary to an RTCStats object of an RTCStatsReport.
// The CalculatedStats object contains additional metrics associated with the
// original RTCStats object. Typically, the RTCStats object contains
// accumulative counters, but in chrome://webrc-internals/ we also want graphs
// for the average rate over the last second, so we have CalculatedStats
// containing calculated Metrics.
class CalculatedStats {
  constructor(id) {
    this.id = id;
    // A map Original Name -> Array of Metrics, where Original Name refers to
    // the name of the metric in the original RTCStats object, and the Metrics
    // are calculated metrics. For example, if the original RTCStats report
    // contains framesReceived, and from that we've calculated
    // [framesReceived/s] and [framesReceived-framesDecoded], then there will be
    // a mapping from "framesReceived" to an array of two Metric objects,
    // "[framesReceived/s]" and "[framesReceived-framesDecoded]".
    this.calculatedMetricsByOriginalName = new Map();
  }

  addCalculatedMetric(originalName, metric) {
    let calculatedMetrics =
        this.calculatedMetricsByOriginalName.get(originalName);
    if (!calculatedMetrics) {
      calculatedMetrics = [];
      this.calculatedMetricsByOriginalName.set(originalName, calculatedMetrics);
    }
    calculatedMetrics.push(metric);
  }

  // Gets the calculated metrics associated with |originalName| in the order
  // that they were added, or an empty list if there are no associated metrics.
  getCalculatedMetrics(originalName) {
    let calculatedMetrics =
        this.calculatedMetricsByOriginalName.get(originalName);
    if (!calculatedMetrics) {
      return [];
    }
    return calculatedMetrics;
  }

  toString() {
    let str = '{id:"' + this.id + '"';
    for (let originalName of this.calculatedMetricsByOriginalName.keys()) {
      const calculatedMetrics =
          this.calculatedMetricsByOriginalName.get(originalName);
      str += ',' + originalName + ':[';
      for (let i = 0; i < calculatedMetrics.length; i++) {
        str += calculatedMetrics[i].toString();
        if (i + 1 < calculatedMetrics.length) {
          str += ',';
        }
        str += ']';
      }
    }
    str += '}';
    return str;
  }
}

// Contains the metrics of an RTCStatsReport, as well as calculated metrics
// associated with metrics from the original report. Convertible to and from the
// "internal reports" format used by webrtc_internals.js to pass stats from C++
// to JavaScript.
class StatsReport {
  constructor() {
    // Represents an RTCStatsReport. It is a Map RTCStats.id -> RTCStats.
    // https://w3c.github.io/webrtc-pc/#dom-rtcstatsreport
    this.statsById = new Map();
    // RTCStats.id -> CalculatedStats
    this.calculatedStatsById = new Map();
  }

  // |internalReports| is an array, each element represents an RTCStats object,
  // but the format is a little different from the spec. This is the format:
  // {
  //   id: "string",
  //   type: "string",
  //   stats: {
  //     timestamp: <milliseconds>,
  //     values: ["member1", value1, "member2", value2...]
  //   }
  // }
  static fromInternalsReportList(internalReports) {
    const result = new StatsReport();
    internalReports.forEach(internalReport => {
      if (!internalReport.stats || !internalReport.stats.values) {
        return;  // continue;
      }
      const stats = {
        id: internalReport.id,
        type: internalReport.type,
        timestamp: internalReport.stats.timestamp / 1000.0  // ms -> s
      };
      const values = internalReport.stats.values;
      for (let i = 0; i < values.length; i += 2) {
        // Metric "name: value".
        stats[values[i]] = values[i + 1];
      }
      result.statsById.set(stats.id, stats);
    });
    return result;
  }

  toInternalsReportList() {
    const result = [];
    for (let stats of this.statsById.values()) {
      const internalReport = {
        id: stats.id,
        type: stats.type,
        stats: {
          timestamp: stats.timestamp * 1000.0,  // s -> ms
          values: []
        }
      };
      Object.keys(stats).forEach(metricName => {
        if (metricName == 'id' || metricName == 'type' ||
            metricName == 'timestamp') {
          return;  // continue;
        }
        internalReport.stats.values.push(metricName);
        internalReport.stats.values.push(stats[metricName]);
        const calculatedMetrics =
            this.getCalculatedMetrics(stats.id, metricName);
        calculatedMetrics.forEach(calculatedMetric => {
          internalReport.stats.values.push(calculatedMetric.name);
          // Treat calculated metrics that are undefined as 0 to ensure graphs
          // can be created anyway.
          internalReport.stats.values.push(
              calculatedMetric.value ? calculatedMetric.value : 0);
        });
      });
      result.push(internalReport);
    }
    return result;
  }

  toString() {
    let str = '';
    for (let stats of this.statsById.values()) {
      if (str != '') {
        str += ',';
      }
      str += JSON.stringify(stats);
    }
    let str2 = '';
    for (let stats of this.calculatedStatsById.values()) {
      if (str2 != '') {
        str2 += ',';
      }
      str2 += stats.toString();
    }
    return '[original:' + str + '],calculated:[' + str2 + ']';
  }

  get(id) {
    return this.statsById.get(id);
  }

  getByType(type) {
    const result = [];
    for (let stats of this.statsById.values()) {
      if (stats.type == type) {
        result.push(stats);
      }
    }
    return result;
  }

  addCalculatedMetric(id, insertAtOriginalMetricName, name, value) {
    let calculatedStats = this.calculatedStatsById.get(id);
    if (!calculatedStats) {
      calculatedStats = new CalculatedStats(id);
      this.calculatedStatsById.set(id, calculatedStats);
    }
    calculatedStats.addCalculatedMetric(
        insertAtOriginalMetricName, new Metric(name, value));
  }

  getCalculatedMetrics(id, originalMetricName) {
    const calculatedStats = this.calculatedStatsById.get(id);
    return calculatedStats ?
        calculatedStats.getCalculatedMetrics(originalMetricName) :
        [];
  }
}

// Calculates the rate "delta accumulative / delta samples" and returns it. If
// a rate cannot be calculated, such as the metric is missing in the current
// or previous report, undefined is returned.
class RateCalculator {
  constructor(
      accumulativeMetric, samplesMetric, modifier = CalculatorModifier.kNone) {
    this.accumulativeMetric = accumulativeMetric;
    this.samplesMetric = samplesMetric;
    this.modifier = modifier;
  }

  getCalculatedMetricName() {
    if (this.samplesMetric == 'timestamp') {
      return '[' + this.accumulativeMetric + '/s]';
    }
    return '[' + this.accumulativeMetric + '/' + this.samplesMetric +
        this.modifier.postfix + ']';
  }

  calculate(id, previousReport, currentReport) {
    return RateCalculator.calculateRate(
               id, previousReport, currentReport, this.accumulativeMetric,
               this.samplesMetric) *
        this.modifier.multiplier;
  }

  static calculateRate(
      id, previousReport, currentReport, accumulativeMetric, samplesMetric) {
    if (!previousReport || !currentReport) {
      return undefined;
    }
    const previousStats = previousReport.get(id);
    const currentStats = currentReport.get(id);
    if (!previousStats || !currentStats) {
      return undefined;
    }
    const deltaTime = currentStats.timestamp - previousStats.timestamp;
    if (deltaTime <= 0) {
      return undefined;
    }
    // Try to convert whatever the values are to numbers. This gets around the
    // fact that some types that are not supported by base::Value (e.g. uint32,
    // int64, uint64 and double) are passed as strings.
    const previousValue = Number(previousStats[accumulativeMetric]);
    const currentValue = Number(currentStats[accumulativeMetric]);
    if (typeof previousValue != 'number' || typeof currentValue != 'number') {
      return undefined;
    }
    const previousSamples = Number(previousStats[samplesMetric]);
    const currentSamples = Number(currentStats[samplesMetric]);
    if (typeof previousSamples != 'number' ||
        typeof currentSamples != 'number') {
      return undefined;
    }
    const deltaValue = currentValue - previousValue;
    const deltaSamples = currentSamples - previousSamples;
    return deltaValue / deltaSamples;
  }
}

// Looks up codec and payload type from a codecId reference, constructing an
// informative string about which codec is used.
class CodecCalculator {
  getCalculatedMetricName() {
    return '[codec]';
  }

  calculate(id, previousReport, currentReport) {
    const targetStats = currentReport.get(id);
    const codecStats = currentReport.get(targetStats.codecId);
    if (!codecStats) {
      return undefined;
    }
    // If mimeType is 'video/VP8' then codec is 'VP8'.
    const codec =
        codecStats.mimeType.substr(codecStats.mimeType.indexOf('/') + 1);
    return codec + ' (payloadType: ' + codecStats.payloadType + ')';
  }
}

// Calculates "RMS" audio level, which is the average audio level between the
// previous and current report, in the interval [0,1]. Calculated per:
// https://w3c.github.io/webrtc-stats/#dom-rtcinboundrtpstreamstats-totalaudioenergy
class AudioLevelRmsCalculator {
  getCalculatedMetricName() {
    return '[Audio_Level_in_RMS]';
  }

  calculate(id, previousReport, currentReport) {
    const averageAudioLevelSquared = RateCalculator.calculateRate(
        id, previousReport, currentReport, 'totalAudioEnergy',
        'totalSamplesDuration');
    return Math.sqrt(averageAudioLevelSquared);
  }
}

// Calculates "metricA - metricB", only looking at the current report.
class DifferenceCalculator {
  constructor(metricA, metricB) {
    this.metricA = metricA;
    this.metricB = metricB;
  }

  getCalculatedMetricName() {
    return '[' + this.metricA + '-' + this.metricB + ']';
  }

  calculate(id, previousReport, currentReport) {
    const currentStats = currentReport.get(id);
    return currentStats[this.metricA] - currentStats[this.metricB];
  }
}

// Calculates the standard deviation from a totalSquaredSum, totalSum, and
// totalCount. If the standard deviation cannot be calculated, such as the
// metric is missing in the current or previous report, undefined is returned.
class StandardDeviationCalculator {
  constructor(totalSquaredSumMetric, totalSumMetric, totalCount, label) {
    this.totalSquaredSumMetric = totalSquaredSumMetric;
    this.totalSumMetric = totalSumMetric;
    this.totalCount = totalCount;
    this.label = label;
  }

  getCalculatedMetricName() {
    return '[' + this.label + 'StDev_in_ms]';
  }

  calculate(id, previousReport, currentReport) {
    return StandardDeviationCalculator.calculateStandardDeviation(
        id, previousReport, currentReport, this.totalSquaredSumMetric,
        this.totalSumMetric, this.totalCount);
  }

  static calculateStandardDeviation(
      id, previousReport, currentReport, totalSquaredSumMetric, totalSumMetric,
      totalCount) {
    if (!previousReport || !currentReport) {
      return undefined;
    }
    const previousStats = previousReport.get(id);
    const currentStats = currentReport.get(id);
    if (!previousStats || !currentStats) {
      return undefined;
    }
    const deltaCount =
        Number(currentStats[totalCount]) - Number(previousStats[totalCount]);
    if (deltaCount <= 0) {
      return undefined;
    }
    // Try to convert whatever the values are to numbers. This gets around the
    // fact that some types that are not supported by base::Value (e.g. uint32,
    // int64, uint64 and double) are passed as strings.
    const previousSquaredSumValue =
        Number(previousStats[totalSquaredSumMetric]);
    const currentSquaredSumValue = Number(currentStats[totalSquaredSumMetric]);
    if (typeof previousSquaredSumValue != 'number' ||
        typeof currentSquaredSumValue != 'number') {
      return undefined;
    }
    const previousSumValue = Number(previousStats[totalSumMetric]);
    const currentSumValue = Number(currentStats[totalSumMetric]);
    if (typeof previousSumValue != 'number' ||
        typeof currentSumValue != 'number') {
      return undefined;
    }

    const deltaSquaredSum = currentSquaredSumValue - previousSquaredSumValue;
    const deltaSum = currentSumValue - previousSumValue;
    const variance =
        (deltaSquaredSum - Math.pow(deltaSum, 2) / deltaCount) / deltaCount;
    if (variance < 0) {
      return undefined;
    }
    return 1000 * Math.sqrt(variance);
  }
}

// Keeps track of previous and current stats report and calculates all
// calculated metrics.
class StatsRatesCalculator {
  constructor() {
    this.previousReport = null;
    this.currentReport = null;
  }

  addStatsReport(report) {
    this.previousReport = this.currentReport;
    this.currentReport = report;
    this.updateCalculatedMetrics_();
  }

  // Updates all "calculated metrics", which are metrics derived from standard
  // values, such as converting total counters (e.g. bytesSent) to rates (e.g.
  // bytesSent/s).
  updateCalculatedMetrics_() {
    const statsCalculators = [
      {
        type: 'data-channel',
        metricCalculators: {
          messagesSent: new RateCalculator('messagesSent', 'timestamp'),
          messagesReceived: new RateCalculator('messagesReceived', 'timestamp'),
          bytesSent: new RateCalculator('bytesSent', 'timestamp'),
          bytesReceived: new RateCalculator('bytesReceived', 'timestamp'),
        },
      },
      {
        type: 'media-source',
        metricCalculators: {
          totalAudioEnergy: new AudioLevelRmsCalculator(),
        },
      },
      {
        type: 'track',
        metricCalculators: {
          framesSent: new RateCalculator('framesSent', 'timestamp'),
          framesReceived: [
            new RateCalculator('framesReceived', 'timestamp'),
            new DifferenceCalculator('framesReceived', 'framesDecoded'),
          ],
          totalAudioEnergy: new AudioLevelRmsCalculator(),
          jitterBufferDelay: new RateCalculator(
              'jitterBufferDelay', 'jitterBufferEmittedCount',
              CalculatorModifier.kMillisecondsFromSeconds),
        },
      },
      {
        type: 'outbound-rtp',
        metricCalculators: {
          bytesSent: new RateCalculator('bytesSent', 'timestamp'),
          packetsSent: new RateCalculator('packetsSent', 'timestamp'),
          totalPacketSendDelay: new RateCalculator(
              'totalPacketSendDelay', 'packetsSent',
              CalculatorModifier.kMillisecondsFromSeconds),
          framesEncoded: new RateCalculator('framesEncoded', 'timestamp'),
          totalEncodedBytesTarget:
              new RateCalculator('totalEncodedBytesTarget', 'timestamp'),
          totalEncodeTime: new RateCalculator(
              'totalEncodeTime', 'framesEncoded',
              CalculatorModifier.kMillisecondsFromSeconds),
          qpSum: new RateCalculator('qpSum', 'framesEncoded'),
          codecId: new CodecCalculator(),
        },
      },
      {
        type: 'inbound-rtp',
        metricCalculators: {
          bytesReceived: new RateCalculator('bytesReceived', 'timestamp'),
          packetsReceived: new RateCalculator('packetsReceived', 'timestamp'),
          framesDecoded: new RateCalculator('framesDecoded', 'timestamp'),
          totalDecodeTime: new RateCalculator(
              'totalDecodeTime', 'framesDecoded',
              CalculatorModifier.kMillisecondsFromSeconds),
          totalInterFrameDelay: new RateCalculator(
              'totalInterFrameDelay', 'framesDecoded',
              CalculatorModifier.kMillisecondsFromSeconds),
          totalSquaredInterFrameDelay: new StandardDeviationCalculator(
              'totalSquaredInterFrameDelay', 'totalInterFrameDelay',
              'framesDecoded', 'interFrameDelay'),
          qpSum: new RateCalculator('qpSum', 'framesDecoded'),
          codecId: new CodecCalculator(),
        },
      },
      {
        type: 'transport',
        metricCalculators: {
          bytesSent: new RateCalculator('bytesSent', 'timestamp'),
          bytesReceived: new RateCalculator('bytesReceived', 'timestamp'),
          // TODO(https://crbug.com/webrtc/10568): Add packetsSent and
          // packetsReceived once implemented.
        },
      },
      {
        type: 'candidate-pair',
        metricCalculators: {
          bytesSent: new RateCalculator('bytesSent', 'timestamp'),
          bytesReceived: new RateCalculator('bytesReceived', 'timestamp'),
          // TODO(https://crbug.com/webrtc/10569): Add packetsSent and
          // packetsReceived once implemented.
          requestsSent: new RateCalculator('requestsSent', 'timestamp'),
          requestsReceived: new RateCalculator('requestsReceived', 'timestamp'),
          responsesSent: new RateCalculator('responsesSent', 'timestamp'),
          responsesReceived:
              new RateCalculator('responsesReceived', 'timestamp'),
          consentRequestsSent:
              new RateCalculator('consentRequestsSent', 'timestamp'),
          consentRequestsReceived:
              new RateCalculator('consentRequestsReceived', 'timestamp'),
          totalRoundTripTime: new RateCalculator(
              'totalRoundTripTime', 'responsesReceived',
              CalculatorModifier.kMillisecondsFromSeconds),
        },
      },
    ];
    statsCalculators.forEach(statsCalculator => {
      this.currentReport.getByType(statsCalculator.type).forEach(stats => {
        Object.keys(statsCalculator.metricCalculators)
            .forEach(originalMetric => {
              let metricCalculators =
                  statsCalculator.metricCalculators[originalMetric];
              if (!Array.isArray(metricCalculators)) {
                metricCalculators = [metricCalculators];
              }
              metricCalculators.forEach(metricCalculator => {
                this.currentReport.addCalculatedMetric(
                    stats.id, originalMetric,
                    metricCalculator.getCalculatedMetricName(),
                    metricCalculator.calculate(
                        stats.id, this.previousReport, this.currentReport));
              });
            });
      });
    });
  }
}