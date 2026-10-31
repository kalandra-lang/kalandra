import unittest
from macos_link_imports import external_symbols, classify


class MacImportsTests(unittest.TestCase):
    def test_own_and_linker_symbols_never_become_system_exports(self):
        text = ('_malloc U 0 0\n_own U 0 0\n_own T 10 4\n'
                '_kalandra_missing U 0 0\n__mlir_ciface_entry U 0 0\n'
                'section$start$__TEXT$__kalandra U 0 0\n___dso_handle U 0 0\n')
        self.assertEqual(external_symbols(text), {'_malloc'})

    def test_unsupported_or_missing_library_names_fail(self):
        result = classify({'_malloc', '__Znwm'}, lambda lib, name:
                          (lib, name) in {('libSystem', 'malloc'), ('libc++', '_Znwm')})
        self.assertEqual(result, {'libSystem': ['_malloc'], 'libc++': ['__Znwm']})
        for symbol in ('_not_a_real_export', 'missing_macho_prefix'):
            with self.subTest(symbol=symbol), self.assertRaises(ValueError):
                classify({symbol}, lambda lib, name: False)


if __name__ == '__main__': unittest.main()
