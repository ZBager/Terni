"use strict";

// Linux desktop rows from bench/charts/results.csv. Times are in milliseconds.
const tiers = [
  { pops: 1_024, compute: 0.0227, ui: null, full: 0.0609, cpu: 0.0284 },
  { pops: 50_000, compute: 0.0312, ui: null, full: 0.1405, cpu: 0.2613 },
  { pops: 1_000_000, compute: 0.1606, ui: 0.1836, full: 1.5452, cpu: 3.9671 },
  { pops: 6_000_000, compute: 1.3267, ui: null, full: 9.6175, cpu: 20.8387 },
  { pops: 10_000_000, compute: 2.2762, ui: null, full: 15.0182, cpu: 35.1315 },
  { pops: 50_000_000, compute: 11.202, ui: null, full: 74.2668, cpu: 163.0382 },
  { pops: 100_000_000, compute: 22.4535, ui: 22.498, full: 151.6937, cpu: 317.5266 },
];

const modes = {
  compute: {
    key: "compute",
    title: "GPU compute time",
    detail: "GPU rules only",
    note: "Excludes GPU transfers and other tick overhead. The CPU bar shows its full tick.",
  },
  ui: {
    key: "ui",
    title: "Full tick time",
    detail: "GPU with UI readback",
    note: "Reads back only the data needed for the map and interface.",
  },
  pops: {
    key: "full",
    title: "Full tick time",
    detail: "GPU with full readback",
    note: "Includes reading every pop back to the CPU.",
  },
};

const controls = document.querySelector("#benchmark-controls");
const slider = document.querySelector("#world-size");
const population = document.querySelector("#world-pops");
const title = document.querySelector("#benchmark-metric");
const detail = document.querySelector("#gpu-detail");
const note = document.querySelector("#benchmark-mode-note");
const summary = document.querySelector("#benchmark-summary");
const chart = controls.closest(".benchmark-chart");
const axis = chart.querySelectorAll(".benchmark-axis span");
const gpuRow = chart.querySelector(".benchmark-row.gpu");
const cpuRow = chart.querySelector(".benchmark-row.cpu");
const english = new Intl.NumberFormat("en", { maximumFractionDigits: 4 });

function formatTime(value) {
  if (value === null) return "N/A";
  return `${value.toFixed(value < 1 ? 3 : 2)} ms`;
}

function updateBar(row, value, maximum) {
  row.querySelector("strong").textContent = formatTime(value);
  row.querySelector(".bar-track span").style.width = value === null ? "0%" : `${value / maximum * 100}%`;
}

function renderBenchmark() {
  const tier = tiers[slider.valueAsNumber];
  const mode = modes[controls.elements.readback.value];
  const gpu = tier[mode.key];

  const worldLabel = `${english.format(tier.pops)} pops`;
  population.value = worldLabel;
  slider.setAttribute("aria-valuetext", worldLabel);
  title.textContent = mode.title;
  detail.textContent = mode.detail;
  note.textContent = gpu === null
    ? "UI readback was measured at 1 million and 100 million pops."
    : mode.note;

  const quarter = Math.max(gpu ?? 0, tier.cpu) / 4;
  const magnitude = 10 ** Math.floor(Math.log10(quarter));
  const step = [1, 2, 2.5, 5, 10].find(candidate => candidate * magnitude >= quarter) * magnitude;
  const maximum = step * 4;
  axis.forEach((tick, index) => {
    tick.textContent = english.format(step * index) + (index === axis.length - 1 ? " ms" : "");
  });
  updateBar(gpuRow, gpu, maximum);
  updateBar(cpuRow, tier.cpu, maximum);

  const gpuResult = gpu === null ? "RTX 4070 Ti UI readback unavailable" : `RTX 4070 Ti ${formatTime(gpu)}`;
  summary.textContent = `${worldLabel}. ${gpuResult}. Ryzen 7 7800X3D ${formatTime(tier.cpu)}.`;
}

slider.addEventListener("input", renderBenchmark);
controls.addEventListener("change", renderBenchmark);
renderBenchmark();
controls.hidden = false;
