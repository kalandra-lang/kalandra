from "desktop" import WindowSize, windowCreate, windowReady, windowTitle, windowText, windowColor, windowShow, windowNextEvent
from "std/process" import ExitStatus

fn main() -> ExitStatus:
  window = windowCreate(WindowSize {width: 540, height: 300})
  if windowReady(window) == 0:
    return ExitStatus {code: 1}
  windowTitle(window, "Kalandra")
  windowText(window, "Hello, world!")
  windowColor(window, 3426421)
  windowShow(window)
  running = true
  while running:
    event = windowNextEvent(window)
    if event == 1 or event == 2:
      running = false
  return ExitStatus {code: 0}
