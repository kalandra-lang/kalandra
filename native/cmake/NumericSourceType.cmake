# Qualification labels may retain their historical names in CI test names.
# Generated Kalandra source always uses the current primitive spellings.
if(DEFINED NUMERIC_TYPE)
  string(REGEX REPLACE "^uint" "u" NUMERIC_TYPE "${NUMERIC_TYPE}")
  string(REGEX REPLACE "^int" "i" NUMERIC_TYPE "${NUMERIC_TYPE}")
  string(REGEX REPLACE "^float" "f" NUMERIC_TYPE "${NUMERIC_TYPE}")
endif()
