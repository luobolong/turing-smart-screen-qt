"""Generate the editable, compact English 480x320 Nightfall dashboard."""
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADER_BASELINE = 65
SYMBOLS = {"cpu": 0xF0EE0, "gauge": 0xF04C5, "temperature": 0xF050F, "memory": 0xF035B,
           "gpu": 0xF0379, "download": 0xF01DA, "upload": 0xF0552,
           "fan": 0xF0210, "power": 0xF06D5, "frequency": 0xF095B, "nixos": 0xF313,
           "network": 0xF059F}
COLORS = {
    "background": "#0B1018", "panel": "#121C29", "border": "#263446",
    "text": "#F2F5FA", "muted": "#A2B2C6", "system": "#7EBAE4",
    "cpu": "#78D8EA", "gpu": "#B5B7FA", "ram": "#E4C48B", "net": "#75D8AF",
    "weather": "#C9D2F3", "track": "#2B3543",
}


def rgba(color):
    color = COLORS.get(color, color).lstrip("#")
    return [int(color[i:i + 2], 16) for i in (0, 2, 4)] + [255]


def layer(key, kind, x, y, w, h, **properties):
    return dict(id="nightfall-" + key, kind=kind, name=key, x=x, y=y, w=w, h=h,
                visible=True, locked=False, **properties)


def text(key, value, x, bottom, w, size=12, color="text", metric=None, align="left", bold=False,
         value_format=None, height=None, family="Turing Screen Latin", valign="bottom", baseline=None,
         shrink_to_fit=False):
    # Text rows share a fixed baseline, with space below for descenders.
    height = height or size + 4
    baseline = valign == "bottom" if baseline is None else baseline
    result = layer(key, "sensor" if metric else "text", x, bottom - height, w, height + (4 if baseline else 0),
                   metric=metric or "", text=value, align=align, valign=valign, clip=True, elide=True,
                   long_text=True, length=0,
                   font=dict(family=family, size=size, color=rgba(color), spacing=0, bold=bold))
    if baseline:
        result["baseline"] = height
    if shrink_to_fit:
        result["shrink_to_fit"] = True
    if value_format:
        result["value_format"] = value_format
    return result


def icon(key, symbol, x, bottom, color="muted", size=16, metric=None, height=None, valign="bottom"):
    result = text(key, chr(SYMBOLS[symbol]) if symbol else "", x, bottom, size, size, color,
                  align="center", height=height or size, metric=metric, valign=valign, baseline=False)
    result["font"]["family"] = "Turing Screen Symbols"
    result.update(elide=False, fit=True, missing_text="")
    return result


def group(key, x, y, children):
    return layer(key, "group", x, y, 0, 0, children=children)


def panel(key, x, y, h, children):
    background = layer(key + "-panel", "rectangle", 0, 0, 222, h,
                       fill=rgba("panel"), stroke=rgba("border"), stroke_width=1)
    return group(key, x, y, [background, *children])


system = group("SYSTEM", 12, 15, [
    icon("system-logo", None, 0, HEADER_BASELINE - 15, "system", size=48, metric="OsIcon"),
    text("system-version", "", 56, 20, 98, 15, "muted", metric="OsName", shrink_to_fit=True),
    text("system-kernel", "", 56, HEADER_BASELINE - 15, 110, 12, "muted", metric="KernelVersion",
         shrink_to_fit=True),
])

clock = group("CLOCK", 166, 7, [
    text("clock-time", "", 0, 35, 148, 30, metric="TimeSeconds", align="center", bold=True,
         family="Turing Screen Mono"),
    text("clock-calendar", "", 0, HEADER_BASELINE - 7, 148, 12, "muted", metric="DateWeekday", align="center"),
])

weather_icon = icon("weather-icon", None, 32, 44, "weather", size=30, metric="WeatherIcon")
weather_icon["align"] = "left"
weather = group("WEATHER", 318, 4, [
    weather_icon,
    text("weather-temperature", "", 56, 44, 94, 28, "weather", metric="WeatherTemp", bold=True, align="right",
         value_format=dict(decimals=0, suffix="°C")),
    text("weather-condition", "", 32, HEADER_BASELINE - 4, 54, 11, "weather", metric="WeatherCondition"),
    text("weather-city", "", 86, HEADER_BASELINE - 4, 64, 11, "weather", metric="WeatherCity", align="right"),
])

cpu = panel("CPU", 12, 80, 138, [
    icon("cpu-chip", "cpu", 12, 23, "cpu", size=14),
    text("cpu-heading", "CPU", 32, 23, 30, 13, "cpu", bold=True),
    text("cpu-model", "AMD Ryzen 7 9800X3D", 68, 23, 142, 11, "muted", align="right"),
    icon("cpu-gauge", "gauge", 12, 66, "cpu", size=17),
    text("cpu-load", "", 34, 66, 76, 36, metric="CpuLoad", align="right", bold=True,
         value_format=dict(decimals=0)),
    text("cpu-percent", "%", 114, 66, 14, 14, "muted"),
    icon("cpu-temp-icon", "temperature", 134, 66, "cpu", size=13),
    text("cpu-temp", "", 150, 66, 60, 20, "cpu", metric="CpuTemp", align="right",
         value_format=dict(decimals=0, suffix="°C")),
    # Center the frequency between load baseline (66) and padded panel bottom (126).
    icon("cpu-frequency-icon", "frequency", 12, 126, size=15, height=60, valign="center"),
    text("cpu-frequency", "", 35, 126, 175, 20, metric="CpuFreq", align="right", height=60, valign="center",
         value_format=dict(divisor=1000, decimals=2, suffix=" GHz")),
])

gpu = panel("GPU", 246, 80, 138, [
    icon("gpu-monitor", "gpu", 12, 23, "gpu", size=14),
    text("gpu-heading", "GPU", 32, 23, 30, 13, "gpu", bold=True),
    text("gpu-model", "AMD Radeon RX 7900 XT", 68, 23, 142, 11, "muted", align="right"),
    icon("gpu-gauge", "gauge", 12, 66, "gpu", size=17),
    text("gpu-load", "", 34, 66, 76, 36, metric="GpuLoad", align="right", bold=True,
         value_format=dict(decimals=0)),
    text("gpu-percent", "%", 114, 66, 14, 14, "muted"),
    icon("gpu-temp-icon", "temperature", 134, 66, "gpu", size=13),
    text("gpu-temp", "", 150, 66, 60, 20, "gpu", metric="GpuTemp", align="right",
         value_format=dict(decimals=0, suffix="°C")),
    icon("gpu-power-icon", "power", 12, 93, size=13),
    text("gpu-power", "", 31, 93, 78, 15, metric="GpuPower", align="right",
         value_format=dict(decimals=0, suffix=" W")),
    icon("gpu-fan-icon", "fan", 112, 93, size=13),
    text("gpu-fan", "", 131, 93, 79, 15, metric="GpuFan", align="right"),
    icon("gpu-vram-icon", "memory", 12, 115, size=13),
    text("gpu-vram", "", 31, 115, 179, 14, metric="GpuRamUsage", align="right"),
    layer("gpu-vram-bar", "bar", 12, 121, 198, 5, metric="GpuRamLoad", step=1,
          front=rgba("gpu"), back=rgba("track"), corner_radius=2.5),
])

ram = panel("RAM", 12, 226, 82, [
    icon("ram-chip", "memory", 12, 23, "ram", size=15),
    text("ram-heading", "RAM", 34, 23, 52, 14, "ram", bold=True),
    text("ram-usage", "", 12, 57, 198, 22, metric="RamUsage", bold=True),
    layer("ram-usage-bar", "bar", 12, 65, 198, 6, metric="RamLoad", step=1,
          front=rgba("ram"), back=rgba("track"), corner_radius=3),
])

net = panel("NET", 246, 226, 82, [
    icon("net-network-icon", "network", 12, 23, "net", size=15),
    text("net-heading", "NET", 34, 23, 35, 14, "net", bold=True),
    text("net-ip", "", 76, 23, 134, 11, "muted", metric="LocalIp", align="right"),
    icon("net-down-icon", "download", 12, 52, "net", size=15),
    text("net-down", "", 39, 52, 171, 16, "net", metric="InternetDown", align="right",
         value_format=dict(auto_unit="bytes/s", decimals=1)),
    icon("net-up-icon", "upload", 12, 73, size=15),
    text("net-up", "", 39, 73, 171, 16, metric="InternetUp", align="right",
         value_format=dict(auto_unit="bytes/s", decimals=1)),
])

theme = dict(format="turing-screen-qt", version=1, name="Nightfall",
             description="Compact English dashboard with system, calendar and IP-based weather.",
             landscape=True, orientation=2, background_color=rgba("background"),
             weather=dict(enabled=True, city=""), layers=[system, clock, weather, cpu, gpu, ram, net])
target = ROOT / "assets/themes/nightfall.json"
target.parent.mkdir(parents=True, exist_ok=True)
target.write_text(json.dumps(theme, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
print(target)
