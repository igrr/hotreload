import pytest
from pytest_embedded import Dut


@pytest.mark.parametrize('target', ['esp32'], indirect=True)
def test_unity_tests_pass(dut: Dut):
    """Verify that all Unity tests pass."""
    # Wait for Unity test output
    dut.expect_exact("=== Running Unity Tests ===", timeout=30)

    # Expect all tests to pass - Unity outputs summary at end
    # Format: "X Tests X Failures X Ignored"
    dut.expect(r"(\d+) Tests (\d+) Failures (\d+) Ignored", timeout=60)

    # Verify we see the completion message
    dut.expect_exact("=== Unity Tests Complete ===", timeout=10)

    # Note: reloadable_hello() will crash until ELF loader is implemented
    # because the symbol table is uninitialized (NULL function pointers)
