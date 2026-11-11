"""Portable application lookup and the inherited environment for qualification."""

SOURCE = '''from "std/env" import get, EnvValue, EnvError
from "std/io" import println, IoError
from "std/parse" import parseU16, ParseError
from "std/fmt" import format

fn describe(name: String) -> String ! EnvError:
  value = try get(name)
  return match value:
    EnvValue.Missing: "missing"
    EnvValue.Value {value}: format("value={}", value)

fn main() -> unit ! (EnvError | IoError | ParseError):
  try println(try describe("KALANDRA_QUAL_ENV_TEXT"))
  try println(try describe("KALANDRA_QUAL_ENV_EMPTY"))
  try println(try describe("KALANDRA_QUAL_ENV_MISSING"))
  try println(try describe("KALANDRA_QUAL_ENV_case"))
  port = try get("KALANDRA_QUAL_ENV_PORT")
  number = match port:
    EnvValue.Missing: u16(8080)
    EnvValue.Value {value}: try parseU16(value)
  try println(format("port={}", number))
  return ()
'''

VARIABLES = {
    "KALANDRA_QUAL_ENV_TEXT": "Grüße=日本語",
    "KALANDRA_QUAL_ENV_EMPTY": "",
    "KALANDRA_QUAL_ENV_CASE": "case match",
    "KALANDRA_QUAL_ENV_PORT": "9000",
    "KALANDRA_QUAL_ENV_MISSING": None,
}


def with_environment(environment, values=VARIABLES, *, windows=False):
    result = dict(environment)
    for name, value in values.items():
        if windows:
            for key in tuple(result):
                if key.upper() == name.upper():
                    del result[key]
        else:
            result.pop(name, None)
        if value is not None:
            result[name] = value
    return result


def expected(windows):
    case = "value=case match" if windows else "missing"
    return f"value=Grüße=日本語\nvalue=\nmissing\n{case}\nport=9000\n"
