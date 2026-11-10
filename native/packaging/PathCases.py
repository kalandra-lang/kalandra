"""Target-platform lexical paths through the normal installed application ABI."""
SOURCE = r'''from "std/path" import join, fileName, parentPath, extension, isAbsolute
from "std/io" import println, IoError
from "std/fmt" import format
fn build(parts:[String])->String:
  return join(parts)
fn main()->unit ! IoError:
  path=build(["data","runs","output.csv"])
  try println(path)
  try println(fileName(path))
  try println(parentPath(path))
  try println(extension(path))
  try println(format("{}:{}:{}:{}",isAbsolute(path),isAbsolute("/root"),isAbsolute("C:/root"),isAbsolute("C:leaf")))
  try println(join(["base","..","last"]))
  try println(extension(".gitignore"))
  try println(extension(".config.json"))
  return ()
'''
def expected(windows):
    sep='\\' if windows else '/'
    return (f'data{sep}runs{sep}output.csv\noutput.csv\ndata{sep}runs\n.csv\n'+
            ('false:false:true:false\n' if windows else 'false:true:false:false\n')+
            f'base{sep}..{sep}last\n\n.json\n')
