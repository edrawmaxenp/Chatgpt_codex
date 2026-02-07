const form = document.querySelector("#buck-form");
const resetButton = document.querySelector("#reset");

const outputs = {
  duty: document.querySelector("#duty"),
  dl: document.querySelector("#dl"),
  l: document.querySelector("#l"),
  ipeak: document.querySelector("#ipeak"),
  c: document.querySelector("#c"),
  ic: document.querySelector("#ic"),
};
const status = document.querySelector("#status");

const formatNumber = (value, unit, digits = 2) => {
  if (!Number.isFinite(value)) {
    return "-";
  }
  return `${value.toFixed(digits)} ${unit}`.trim();
};

const calculate = () => {
  const vin = Number.parseFloat(form.vin.value);
  const vout = Number.parseFloat(form.vout.value);
  const iout = Number.parseFloat(form.iout.value);
  const fswKhz = Number.parseFloat(form.fsw.value);
  const ripplePercent = Number.parseFloat(form.ripple.value);
  const vRippleMv = Number.parseFloat(form.vipple.value);

  if (vin <= 0 || vout <= 0 || iout <= 0 || fswKhz <= 0 || ripplePercent <= 0 || vRippleMv <= 0) {
    Object.values(outputs).forEach((node) => {
      node.textContent = "-";
    });
    status.textContent = "Enter positive values for all fields to calculate results.";
    status.className = "status error";
    return;
  }

  const duty = vout / vin;
  if (duty >= 1) {
    Object.values(outputs).forEach((node) => {
      node.textContent = "-";
    });
    status.textContent = "Vout must be lower than Vin for a buck converter. Adjust the inputs.";
    status.className = "status error";
    return;
  }

  const fsw = fswKhz * 1000;
  const deltaIL = (ripplePercent / 100) * iout;
  const inductance = ((vin - vout) * duty) / (deltaIL * fsw);
  const inductorPeak = iout + deltaIL / 2;
  const vRipple = vRippleMv / 1000;
  const capacitance = deltaIL / (8 * fsw * vRipple);
  const capRippleCurrent = deltaIL / (2 * Math.sqrt(3));

  outputs.duty.textContent = formatNumber(duty * 100, "%", 1);
  outputs.dl.textContent = formatNumber(deltaIL, "A", 3);
  outputs.l.textContent = formatNumber(inductance * 1e6, "µH", 2);
  outputs.ipeak.textContent = formatNumber(inductorPeak, "A", 3);
  outputs.c.textContent = formatNumber(capacitance * 1e6, "µF", 1);
  outputs.ic.textContent = formatNumber(capRippleCurrent, "A", 3);

  if (duty > 0.9) {
    status.textContent = "Duty cycle is high (>90%). Verify switch losses and headroom.";
    status.className = "status warning";
  } else {
    status.textContent = "Calculation complete. Review component ratings for margins.";
    status.className = "status";
  }
};

form.addEventListener("submit", (event) => {
  event.preventDefault();
  calculate();
});

form.addEventListener("input", () => {
  calculate();
});

resetButton.addEventListener("click", () => {
  form.reset();
  calculate();
});

calculate();
