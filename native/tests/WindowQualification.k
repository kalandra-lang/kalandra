from "desktop" import WindowSize, windowCreate, windowReady, windowTitle, windowText, windowColor, windowShow, windowNextEvent, windowResize, windowWidth, windowPostKey, windowLive
from "std/process" import ExitStatus

fn cycle() -> bool:
  w = windowCreate(WindowSize {width: 380, height: 220})
  if windowReady(w) == 0:
    return false
  windowTitle(w, "Kalandra")
  windowText(w, "Hello, 世界!")
  windowColor(w, 3426421)
  windowShow(w)
  windowResize(w, WindowSize {width: 480, height: 300})
  windowPostKey(w)
  resized = false
  key = false
  for i in range(200):
    event = windowNextEvent(w)
    if event == 1:
      key = true
    if event == 3 and windowWidth(w) == 480:
      resized = true
    if key and resized:
      return true
  return false

fn main() -> ExitStatus:
  for i in range(3):
    if cycle() == false or windowLive() != 0:
      return ExitStatus {code: 1}
  return ExitStatus {code: 0}
