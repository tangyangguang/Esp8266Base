#!/usr/bin/env python3
"""Test exact limits and fail-closed parsing without a compiler or device."""
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from check_resource_budget import MAX_RAM_BYTES, check_elf, main, parse_size_output


def size_output(used):
    return ("firmware.elf :\nsection size addr\n"
            ".data 100 1073643520\n.rodata 200 1073643620\n"
            f".bss {used - 300} 1073643820\n"
            ".irom0.text 500000 1075843088\n"
            ".text 10000 1074790400\nTotal 1000000\n")


class ResourceBudgetTests(unittest.TestCase):
    def test_exact_integer_limit_not_rounded_percent(self):
        self.assertEqual(MAX_RAM_BYTES, 54067)
        self.assertTrue(parse_size_output(size_output(54067))["passed"])
        self.assertFalse(parse_size_output(size_output(54068))["passed"])

    def test_accounting_matches_platformio_ram_sections(self):
        report = parse_size_output(size_output(53376))
        self.assertEqual(report["ram_bytes"], 53376)
        self.assertEqual(report["scope"], "static_only")
        self.assertTrue(report["passed"])

    def test_malformed_or_incomplete_output_is_rejected(self):
        good = size_output(53376)
        cases = ["", good.replace(".rodata 200 1073643620\n", ""),
                 good + ".bss 10 1073643820\n",
                 good.replace(".data 100", ".data -1"),
                 good.replace(".data 100", ".data 1.0"),
                 good.replace("1073643520", "0"),
                 good.replace(".bss 53076", ".bss 999999999"),
                 good.replace(".irom0.text", ".flash.text"),
                 good.replace("1075843088", "0")]
        for text in cases:
            with self.subTest(text=text):
                with self.assertRaises(ValueError):
                    parse_size_output(text)

    def test_cli_returns_failure_for_over_budget_and_invalid_input(self):
        with patch("sys.argv", ["check", "--elf", "unused", "--size-tool", "unused"]):
            with patch("builtins.print"):
                with patch("check_resource_budget.check_elf", return_value={"passed": False}):
                    self.assertEqual(main(), 1)
                with patch("check_resource_budget.check_elf", side_effect=ValueError("invalid")):
                    self.assertEqual(main(), 2)
                with patch("check_resource_budget.check_elf", return_value={"passed": True}):
                    self.assertEqual(main(), 0)

    def test_invalid_elf_fails_before_running_tool(self):
        with tempfile.TemporaryDirectory() as temp:
            elf = Path(temp) / "firmware.elf"
            for header in (b"", b"not ELF" * 4, b"\x7fELF\x02\x01" + bytes(14)):
                elf.write_bytes(header)
                with patch("check_resource_budget.subprocess.run") as run:
                    with self.assertRaises(ValueError):
                        check_elf(elf, "unused")
                    run.assert_not_called()


if __name__ == "__main__":
    unittest.main()
