from __future__ import annotations
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from fleaux_cpp_transpiler import FleauxCppTranspiler
class CppBackendTests(unittest.TestCase):
    def test_cpp_backend_runs_simple_program(self) -> None:
        if shutil.which("c++") is None:
            self.skipTest("c++ compiler is required for cpp backend test")
        repo_root = Path(__file__).resolve().parents[1]
        launcher = repo_root / "fleaux"
        with tempfile.TemporaryDirectory() as tmp_dir:
            source = Path(tmp_dir) / "cpp_backend_test.fleaux"
            source.write_text(
                "import Std;\n"
                "(1, 2) -> Std.Add -> Std.Println;\n",
                encoding="utf-8",
            )
            completed = subprocess.run(
                [str(launcher), str(source), "--backend", "cpp"],
                cwd=tmp_dir,
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(completed.returncode, 0, completed.stderr)
            self.assertIn("3", completed.stdout)
    def test_cpp_backend_generates_cpp_file(self) -> None:
        if shutil.which("c++") is None:
            self.skipTest("c++ compiler is required for cpp backend test")
        repo_root = Path(__file__).resolve().parents[1]
        launcher = repo_root / "fleaux"
        with tempfile.TemporaryDirectory() as tmp_dir:
            source = Path(tmp_dir) / "emit_cpp_only.fleaux"
            source.write_text(
                "import Std;\n"
                "(3, 4) -> Std.Add;\n",
                encoding="utf-8",
            )
            completed = subprocess.run(
                [str(launcher), str(source), "--backend", "cpp"],
                cwd=tmp_dir,
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(completed.returncode, 0, completed.stderr)
            generated = Path(tmp_dir) / "fleaux_generated_module_emit_cpp_only.cpp"
            self.assertTrue(generated.exists())

    def test_cpp_backend_loop_builtin(self) -> None:
        if shutil.which("c++") is None:
            self.skipTest("c++ compiler is required for cpp backend test")

        repo_root = Path(__file__).resolve().parents[1]
        launcher = repo_root / "fleaux"

        with tempfile.TemporaryDirectory() as tmp_dir:
            source = Path(tmp_dir) / "loop_cpp_backend_test.fleaux"
            source.write_text(
                "import Std;\n"
                "let Continue(n: Number, acc: Number): Bool = (n, 0) -> Std.GreaterThan;\n"
                "let Step(n: Number, acc: Number): Tuple(Number, Number) =\n"
                "    ((n, 1) -> Std.Subtract, (acc, n) -> Std.Add);\n"
                "let SumToLoop(n: Number): Number =\n"
                "    ((n, 0), Continue, Step) -> Std.Loop -> (_, 1) -> Std.ElementAt;\n"
                "(5) -> SumToLoop -> Std.Println;\n",
                encoding="utf-8",
            )
            completed = subprocess.run(
                [str(launcher), str(source), "--backend", "cpp"],
                cwd=tmp_dir,
                text=True,
                capture_output=True,
                check=False,
            )

            self.assertEqual(completed.returncode, 0, completed.stderr)
            self.assertIn("15", completed.stdout)

    def test_cpp_backend_string_builtins(self) -> None:
        if shutil.which("c++") is None:
            self.skipTest("c++ compiler is required for cpp backend test")

        repo_root = Path(__file__).resolve().parents[1]
        launcher = repo_root / "fleaux"

        with tempfile.TemporaryDirectory() as tmp_dir:
            source = Path(tmp_dir) / "string_cpp_backend_test.fleaux"
            source.write_text(
                "import Std;\n"
                "(\"  hello  \") -> Std.String.Trim -> Std.Println;\n"
                "(\"a,b,c\", \",\") -> Std.String.Split -> (\"-\", _) -> Std.String.Join -> Std.Println;\n"
                "(\"HELLO\", \"he\") -> Std.String.StartsWith -> Std.Println;\n"
                "(\"ababa\", \"ba\", \"XY\") -> Std.String.Replace -> Std.Println;\n",
                encoding="utf-8",
            )
            completed = subprocess.run(
                [str(launcher), str(source), "--backend", "cpp"],
                cwd=tmp_dir,
                text=True,
                capture_output=True,
                check=False,
            )

            self.assertEqual(completed.returncode, 0, completed.stderr)
            self.assertIn("hello", completed.stdout)
            self.assertIn("a-b-c", completed.stdout)
            self.assertIn("False", completed.stdout)
            self.assertIn("aXYXY", completed.stdout)

    def test_cpp_backend_printf_builtin(self) -> None:
        if shutil.which("c++") is None:
            self.skipTest("c++ compiler is required for cpp backend test")

        repo_root = Path(__file__).resolve().parents[1]
        launcher = repo_root / "fleaux"

        with tempfile.TemporaryDirectory() as tmp_dir:
            source = Path(tmp_dir) / "printf_cpp_backend_test.fleaux"
            source.write_text(
                "import Std;\n"
                '("name={}, score={}", "matt", 42) -> Std.Printf;\n',
                encoding="utf-8",
            )
            completed = subprocess.run(
                [str(launcher), str(source), "--backend", "cpp"],
                cwd=tmp_dir,
                text=True,
                capture_output=True,
                check=False,
            )

            self.assertEqual(completed.returncode, 0, completed.stderr)
            self.assertIn("name=matt, score=42", completed.stdout)

    def test_cpp_backend_get_args_builtin(self) -> None:
        if shutil.which("c++") is None:
            self.skipTest("c++ compiler is required for cpp backend test")

        repo_root = Path(__file__).resolve().parents[1]
        launcher = repo_root / "fleaux"

        with tempfile.TemporaryDirectory() as tmp_dir:
            source = Path(tmp_dir) / "getargs_cpp_backend_test.fleaux"
            source.write_text(
                "import Std;\n"
                "() -> Std.GetArgs -> Std.Println;\n",
                encoding="utf-8",
            )
            completed = subprocess.run(
                [str(launcher), str(source), "--backend", "cpp"],
                cwd=tmp_dir,
                text=True,
                capture_output=True,
                check=False,
            )

            self.assertEqual(completed.returncode, 0, completed.stderr)
            self.assertIn("fleaux_generated_module_getargs_cpp_backend_test", completed.stdout)

    def test_cpp_backend_input_builtin_no_prompt(self) -> None:
        if shutil.which("c++") is None:
            self.skipTest("c++ compiler is required for cpp backend test")

        repo_root = Path(__file__).resolve().parents[1]
        launcher = repo_root / "fleaux"

        with tempfile.TemporaryDirectory() as tmp_dir:
            source = Path(tmp_dir) / "input_no_prompt_cpp_backend_test.fleaux"
            source.write_text(
                "import Std;\n"
                "() -> Std.Input -> Std.Println;\n",
                encoding="utf-8",
            )
            completed = subprocess.run(
                [str(launcher), str(source), "--backend", "cpp"],
                cwd=tmp_dir,
                text=True,
                input="hello-from-stdin\n",
                capture_output=True,
                check=False,
            )

            self.assertEqual(completed.returncode, 0, completed.stderr)
            self.assertIn("hello-from-stdin", completed.stdout)

    def test_cpp_backend_input_builtin_with_prompt(self) -> None:
        if shutil.which("c++") is None:
            self.skipTest("c++ compiler is required for cpp backend test")

        repo_root = Path(__file__).resolve().parents[1]
        launcher = repo_root / "fleaux"

        with tempfile.TemporaryDirectory() as tmp_dir:
            source = Path(tmp_dir) / "input_prompt_cpp_backend_test.fleaux"
            source.write_text(
                "import Std;\n"
                '("Enter value: ") -> Std.Input -> Std.Println;\n',
                encoding="utf-8",
            )
            completed = subprocess.run(
                [str(launcher), str(source), "--backend", "cpp"],
                cwd=tmp_dir,
                text=True,
                input="typed-value\n",
                capture_output=True,
                check=False,
            )

            self.assertEqual(completed.returncode, 0, completed.stderr)
            self.assertIn("Enter value:", completed.stdout)
            self.assertIn("typed-value", completed.stdout)

    def test_cpp_backend_input_builtin_wrong_arity(self) -> None:
        if shutil.which("c++") is None:
            self.skipTest("c++ compiler is required for cpp backend test")

        repo_root = Path(__file__).resolve().parents[1]
        launcher = repo_root / "fleaux"

        with tempfile.TemporaryDirectory() as tmp_dir:
            source = Path(tmp_dir) / "input_arity_cpp_backend_test.fleaux"
            source.write_text(
                "import Std;\n"
                '("a", "b") -> Std.Input;\n',
                encoding="utf-8",
            )
            completed = subprocess.run(
                [str(launcher), str(source), "--backend", "cpp"],
                cwd=tmp_dir,
                text=True,
                capture_output=True,
                check=False,
            )

            self.assertNotEqual(completed.returncode, 0)
            self.assertIn("Input expects 0 or 1 argument", completed.stderr)

    def test_cpp_backend_exit_builtin_default_code(self) -> None:
        if shutil.which("c++") is None:
            self.skipTest("c++ compiler is required for cpp backend test")

        repo_root = Path(__file__).resolve().parents[1]
        launcher = repo_root / "fleaux"

        with tempfile.TemporaryDirectory() as tmp_dir:
            source = Path(tmp_dir) / "exit_default_cpp_backend_test.fleaux"
            source.write_text(
                "import Std;\n"
                "() -> Std.Exit;\n",
                encoding="utf-8",
            )
            completed = subprocess.run(
                [str(launcher), str(source), "--backend", "cpp"],
                cwd=tmp_dir,
                text=True,
                capture_output=True,
                check=False,
            )

            self.assertEqual(completed.returncode, 0, completed.stderr)

    def test_cpp_backend_exit_builtin_explicit_code(self) -> None:
        if shutil.which("c++") is None:
            self.skipTest("c++ compiler is required for cpp backend test")

        repo_root = Path(__file__).resolve().parents[1]
        launcher = repo_root / "fleaux"

        with tempfile.TemporaryDirectory() as tmp_dir:
            source = Path(tmp_dir) / "exit_code_cpp_backend_test.fleaux"
            source.write_text(
                "import Std;\n"
                "(7) -> Std.Exit;\n",
                encoding="utf-8",
            )
            completed = subprocess.run(
                [str(launcher), str(source), "--backend", "cpp"],
                cwd=tmp_dir,
                text=True,
                capture_output=True,
                check=False,
            )

            self.assertEqual(completed.returncode, 7, completed.stderr)

    def test_cpp_backend_math_builtins(self) -> None:
        if shutil.which("c++") is None:
            self.skipTest("c++ compiler is required for cpp backend test")

        repo_root = Path(__file__).resolve().parents[1]
        launcher = repo_root / "fleaux"

        with tempfile.TemporaryDirectory() as tmp_dir:
            source = Path(tmp_dir) / "math_cpp_backend_test.fleaux"
            source.write_text(
                "import Std;\n"
                "(3.8) -> Std.Math.Floor -> Std.Println;\n"
                "(3.2) -> Std.Math.Ceil -> Std.Println;\n"
                "(-4) -> Std.Math.Abs -> Std.Println;\n"
                "(10, 0, 3) -> Std.Math.Clamp -> Std.Println;\n",
                encoding="utf-8",
            )
            completed = subprocess.run(
                [str(launcher), str(source), "--backend", "cpp"],
                cwd=tmp_dir,
                text=True,
                capture_output=True,
                check=False,
            )

            self.assertEqual(completed.returncode, 0, completed.stderr)
            self.assertIn("3", completed.stdout)
            self.assertIn("4", completed.stdout)

    def test_cpp_backend_path_file_dir_os_builtins(self) -> None:
        if shutil.which("c++") is None:
            self.skipTest("c++ compiler is required for cpp backend test")

        repo_root = Path(__file__).resolve().parents[1]
        launcher = repo_root / "fleaux"

        with tempfile.TemporaryDirectory() as tmp_dir:
            source = Path(tmp_dir) / "io_cpp_backend_test.fleaux"
            source.write_text(
                "import Std;\n"
                "(\"cpp_backend_io_test.txt\", \"hello\") -> Std.File.WriteText;\n"
                "(\"cpp_backend_io_test.txt\") -> Std.File.ReadText -> Std.Println;\n"
                "(\"cpp_backend_io_test.txt\") -> Std.File.Size -> Std.Println;\n"
                "(\"cpp_backend_io_test.txt\") -> Std.Path.Exists -> Std.Println;\n"
                "(\"cpp_backend_io_test\", \"txt\") -> Std.Path.WithExtension -> Std.Println;\n"
                "(\"cpp_backend_dir\") -> Std.Dir.Create;\n"
                "(\"cpp_backend_dir\") -> Std.Dir.List -> Std.Println;\n"
                "() -> Std.OS.Cwd -> Std.Println;\n"
                "() -> Std.OS.TempDir -> Std.Println;\n",
                encoding="utf-8",
            )
            completed = subprocess.run(
                [str(launcher), str(source), "--backend", "cpp"],
                cwd=tmp_dir,
                text=True,
                capture_output=True,
                check=False,
            )

            self.assertEqual(completed.returncode, 0, completed.stderr)
            self.assertIn("hello", completed.stdout)
            self.assertIn("5", completed.stdout)
            self.assertIn("True", completed.stdout)
            self.assertIn("cpp_backend_io_test.txt", completed.stdout)

    # ── OS.Env / OS.HasEnv / OS.SetEnv / OS.UnsetEnv edge cases ─────────────

    def _run_cpp(self, tmp_dir: str, source_name: str, code: str,
                 extra_env: dict | None = None) -> subprocess.CompletedProcess:
        """Helper: write *code* to a .fleaux file, compile + run with cpp backend."""
        repo_root = Path(__file__).resolve().parents[1]
        launcher = repo_root / "fleaux"
        source = Path(tmp_dir) / source_name
        source.write_text(code, encoding="utf-8")
        env = dict(os.environ)
        if extra_env:
            env.update(extra_env)
        return subprocess.run(
            [str(launcher), str(source), "--backend", "cpp"],
            cwd=tmp_dir,
            text=True,
            capture_output=True,
            check=False,
            env=env,
        )

    def test_cpp_backend_compiles_all_samples(self) -> None:
        if shutil.which("c++") is None:
            self.skipTest("c++ compiler is required for cpp backend test")

        repo_root = Path(__file__).resolve().parents[1]
        samples = sorted((repo_root / "samples").glob("*.fleaux"))
        self.assertGreater(len(samples), 0, "No sample files found in samples/")

        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp_path = Path(tmp_dir)
            for sample in samples:
                shutil.copy2(sample, tmp_path / sample.name)

            for sample in samples:
                with self.subTest(sample=sample.name):
                    local_source = tmp_path / sample.name
                    generated_cpp = FleauxCppTranspiler().process(local_source)
                    binary = generated_cpp.with_suffix("")
                    compile_cmd = [
                        "c++", "-std=c++20", "-O2", str(generated_cpp),
                        "-I", str(repo_root / "cpp"),
                        "-I", str(repo_root / "third_party" / "datatree" / "include"),
                        "-I", str(repo_root / "third_party" / "tl" / "include"),
                        "-o", str(binary),
                    ]
                    compiled = subprocess.run(
                        compile_cmd,
                        cwd=tmp_dir,
                        text=True,
                        capture_output=True,
                        check=False,
                    )
                    self.assertEqual(
                        compiled.returncode,
                        0,
                        f"{sample.name} failed C++ compilation:\n{compiled.stderr}",
                    )

    def test_cpp_backend_runs_all_samples_without_crash(self) -> None:
        if shutil.which("c++") is None:
            self.skipTest("c++ compiler is required for cpp backend test")

        repo_root = Path(__file__).resolve().parents[1]
        launcher = repo_root / "fleaux"
        samples = sorted((repo_root / "samples").glob("*.fleaux"))
        self.assertGreater(len(samples), 0, "No sample files found in samples/")

        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp_path = Path(tmp_dir)
            for sample in samples:
                shutil.copy2(sample, tmp_path / sample.name)

            for sample in samples:
                with self.subTest(sample=sample.name):
                    completed = subprocess.run(
                        [str(launcher), str(tmp_path / sample.name), "--backend", "cpp"],
                        cwd=tmp_dir,
                        text=True,
                        capture_output=True,
                        check=False,
                    )
                    self.assertEqual(
                        completed.returncode,
                        0,
                        f"{sample.name} crashed in cpp backend:\n{completed.stderr}",
                    )

    def test_cpp_backend_samples_logical_smoke(self) -> None:
        """Guard key sample outputs by logic (no strict formatting parity required)."""
        if shutil.which("c++") is None:
            self.skipTest("c++ compiler is required for cpp backend test")

        repo_root = Path(__file__).resolve().parents[1]
        launcher = repo_root / "fleaux"
        checks = {
            "02_arithmetic.fleaux": ["13", "30", "5"],
            "03_pipeline_chaining.fleaux": ["14", "30"],
            "12_math.fleaux": ["256", "100", "0", "50"],
            "13_comparison_and_logic.fleaux": ["true", "false"],
            "17_printf_and_tostring.fleaux": ["42", "3.14"],
        }

        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp_path = Path(tmp_dir)
            for sample in (repo_root / "samples").glob("*.fleaux"):
                shutil.copy2(sample, tmp_path / sample.name)

            for sample_name, expected_snippets in checks.items():
                with self.subTest(sample=sample_name):
                    completed = subprocess.run(
                        [str(launcher), str(tmp_path / sample_name), "--backend", "cpp"],
                        cwd=tmp_dir,
                        text=True,
                        capture_output=True,
                        check=False,
                    )
                    self.assertEqual(completed.returncode, 0, completed.stderr)
                    out_lc = completed.stdout.lower()
                    for snippet in expected_snippets:
                        self.assertIn(snippet.lower(), out_lc)

    def test_cpp_backend_compiles_all_samples(self) -> None:
        if shutil.which("c++") is None:
            self.skipTest("c++ compiler is required for cpp backend test")

        repo_root = Path(__file__).resolve().parents[1]
        samples_dir = repo_root / "samples"
        samples = sorted(samples_dir.glob("*.fleaux"))
        self.assertGreater(len(samples), 0, "No sample files found in samples/")

        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp_path = Path(tmp_dir)

            # Keep relative imports between samples working (e.g. 21_import -> 20_export).
            for sample in samples:
                shutil.copy2(sample, tmp_path / sample.name)

            for sample in samples:
                with self.subTest(sample=sample.name):
                    local_source = tmp_path / sample.name
                    generated_cpp = FleauxCppTranspiler().process(local_source)
                    binary = generated_cpp.with_suffix("")

                    compile_cmd = [
                        "c++",
                        "-std=c++20",
                        "-O2",
                        str(generated_cpp),
                        "-I",
                        str(repo_root / "cpp"),
                        "-I",
                        str(repo_root / "third_party" / "datatree" / "include"),
                        "-I",
                        str(repo_root / "third_party" / "tl" / "include"),
                        "-o",
                        str(binary),
                    ]
                    compiled = subprocess.run(
                        compile_cmd,
                        cwd=tmp_dir,
                        text=True,
                        capture_output=True,
                        check=False,
                    )

                    self.assertEqual(
                        compiled.returncode,
                        0,
                        f"{sample.name} failed C++ compilation:\n{compiled.stderr}",
                    )
                    generated = tmp_path / f"fleaux_generated_module_{sample.stem}.cpp"
                    self.assertTrue(generated.exists(), f"Missing generated C++ file for {sample.name}")

    def test_cpp_backend_runs_all_samples_without_crash(self) -> None:
        if shutil.which("c++") is None:
            self.skipTest("c++ compiler is required for cpp backend test")

        repo_root = Path(__file__).resolve().parents[1]
        launcher = repo_root / "fleaux"
        samples_dir = repo_root / "samples"
        samples = sorted(samples_dir.glob("*.fleaux"))
        self.assertGreater(len(samples), 0, "No sample files found in samples/")

        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp_path = Path(tmp_dir)
            for sample in samples:
                shutil.copy2(sample, tmp_path / sample.name)

            for sample in samples:
                with self.subTest(sample=sample.name):
                    local_source = tmp_path / sample.name
                    completed = subprocess.run(
                        [str(launcher), str(local_source), "--backend", "cpp"],
                        cwd=tmp_dir,
                        text=True,
                        capture_output=True,
                        check=False,
                    )
                    self.assertEqual(
                        completed.returncode,
                        0,
                        f"{sample.name} crashed in cpp backend:\n{completed.stderr}",
                    )

    def test_cpp_backend_os_env_existing_key(self) -> None:
        """OSEnv returns the value string of a key present in the environment."""
        if shutil.which("c++") is None:
            self.skipTest("c++ compiler is required for cpp backend test")
        with tempfile.TemporaryDirectory() as tmp:
            r = self._run_cpp(
                tmp, "os_env_existing.fleaux",
                'import Std;\n'
                '("FLEAUX_CPP_TEST_VAR") -> Std.OS.Env -> Std.Println;\n',
                extra_env={"FLEAUX_CPP_TEST_VAR": "hello_from_env"},
            )
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn("hello_from_env", r.stdout)

    def test_cpp_backend_os_env_missing_key_returns_null(self) -> None:
        """OSEnv returns None (not a string) when the key is absent."""
        if shutil.which("c++") is None:
            self.skipTest("c++ compiler is required for cpp backend test")
        with tempfile.TemporaryDirectory() as tmp:
            r = self._run_cpp(
                tmp, "os_env_missing.fleaux",
                'import Std;\n'
                '("FLEAUX_MISSING_KEY_ZZZZZZZZZZ") -> Std.OS.Env -> Std.Println;\n',
            )
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn("None", r.stdout)

    def test_cpp_backend_os_has_env_true(self) -> None:
        """OSHasEnv returns true for a key present in the environment."""
        if shutil.which("c++") is None:
            self.skipTest("c++ compiler is required for cpp backend test")
        with tempfile.TemporaryDirectory() as tmp:
            r = self._run_cpp(
                tmp, "os_has_env_true.fleaux",
                'import Std;\n'
                '("FLEAUX_PRESENT_VAR") -> Std.OS.HasEnv -> Std.Println;\n',
                extra_env={"FLEAUX_PRESENT_VAR": "1"},
            )
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn("True", r.stdout)

    def test_cpp_backend_os_has_env_false(self) -> None:
        """OSHasEnv returns false for a key absent from the environment."""
        if shutil.which("c++") is None:
            self.skipTest("c++ compiler is required for cpp backend test")
        with tempfile.TemporaryDirectory() as tmp:
            env_without_key = {k: v for k, v in os.environ.items()
                               if k != "FLEAUX_ABSENT_VAR_ZZZZ"}
            source = Path(tmp) / "os_has_env_false.fleaux"
            source.write_text(
                'import Std;\n'
                '("FLEAUX_ABSENT_VAR_ZZZZ") -> Std.OS.HasEnv -> Std.Println;\n',
                encoding="utf-8",
            )
            repo_root = Path(__file__).resolve().parents[1]
            r = subprocess.run(
                [str(repo_root / "fleaux"), str(source), "--backend", "cpp"],
                cwd=tmp, text=True, capture_output=True, check=False,
                env=env_without_key,
            )
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn("False", r.stdout)

    def test_cpp_backend_os_set_env_returns_value(self) -> None:
        """OSSetEnv returns the value string that was set."""
        if shutil.which("c++") is None:
            self.skipTest("c++ compiler is required for cpp backend test")
        with tempfile.TemporaryDirectory() as tmp:
            r = self._run_cpp(
                tmp, "os_set_env_ret.fleaux",
                'import Std;\n'
                '("FLEAUX_SET_VAR", "set_value") -> Std.OS.SetEnv -> Std.Println;\n',
            )
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn("set_value", r.stdout)

    def test_cpp_backend_os_set_env_overwrite(self) -> None:
        """OSSetEnv overwrites an existing variable; subsequent OSEnv sees the new value."""
        if shutil.which("c++") is None:
            self.skipTest("c++ compiler is required for cpp backend test")
        with tempfile.TemporaryDirectory() as tmp:
            r = self._run_cpp(
                tmp, "os_set_env_overwrite.fleaux",
                'import Std;\n'
                '("FLEAUX_OW_VAR", "new_value") -> Std.OS.SetEnv;\n'
                '("FLEAUX_OW_VAR") -> Std.OS.Env -> Std.Println;\n',
                extra_env={"FLEAUX_OW_VAR": "old_value"},
            )
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn("new_value", r.stdout)
            self.assertNotIn("old_value", r.stdout)

    def test_cpp_backend_os_unset_env_missing_key(self) -> None:
        """OSUnsetEnv returns false when the key was not in the environment."""
        if shutil.which("c++") is None:
            self.skipTest("c++ compiler is required for cpp backend test")
        with tempfile.TemporaryDirectory() as tmp:
            env_without_key = {k: v for k, v in os.environ.items()
                               if k != "FLEAUX_UNSET_MISSING_ZZZZ"}
            source = Path(tmp) / "os_unset_missing.fleaux"
            source.write_text(
                'import Std;\n'
                '("FLEAUX_UNSET_MISSING_ZZZZ") -> Std.OS.UnsetEnv -> Std.Println;\n',
                encoding="utf-8",
            )
            repo_root = Path(__file__).resolve().parents[1]
            r = subprocess.run(
                [str(repo_root / "fleaux"), str(source), "--backend", "cpp"],
                cwd=tmp, text=True, capture_output=True, check=False,
                env=env_without_key,
            )
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn("False", r.stdout)

    def test_cpp_backend_os_unset_env_existing_key(self) -> None:
        """OSUnsetEnv returns true for an existing key; subsequent HasEnv returns false."""
        if shutil.which("c++") is None:
            self.skipTest("c++ compiler is required for cpp backend test")
        with tempfile.TemporaryDirectory() as tmp:
            r = self._run_cpp(
                tmp, "os_unset_existing.fleaux",
                'import Std;\n'
                '("FLEAUX_UNSET_PRESENT") -> Std.OS.UnsetEnv -> Std.Println;\n'
                '("FLEAUX_UNSET_PRESENT") -> Std.OS.HasEnv -> Std.Println;\n',
                extra_env={"FLEAUX_UNSET_PRESENT": "removeme"},
            )
            self.assertEqual(r.returncode, 0, r.stderr)
            lines = [ln for ln in r.stdout.splitlines() if ln.strip()]
            self.assertEqual(lines[0], "True",  f"UnsetEnv should return true; got: {r.stdout!r}")
            self.assertEqual(lines[1], "False", f"HasEnv after unset should be false; got: {r.stdout!r}")

    # ── Tuple builtins ────────────────────────────────────────────────────────

    def test_cpp_backend_tuple_append(self) -> None:
        """Std.Tuple.Append appends an element to a tuple."""
        if shutil.which("c++") is None:
            self.skipTest("c++ compiler is required for cpp backend test")
        with tempfile.TemporaryDirectory() as tmp:
            r = self._run_cpp(
                tmp, "tuple_append.fleaux",
                'import Std;\n'
                '((1, 2, 3), 4) -> Std.Tuple.Append -> Std.Println;\n',
            )
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn("4", r.stdout)
            self.assertIn("1", r.stdout)

    def test_cpp_backend_tuple_prepend(self) -> None:
        """Std.Tuple.Prepend prepends an element to a tuple."""
        if shutil.which("c++") is None:
            self.skipTest("c++ compiler is required for cpp backend test")
        with tempfile.TemporaryDirectory() as tmp:
            r = self._run_cpp(
                tmp, "tuple_prepend.fleaux",
                'import Std;\n'
                '((2, 3), 1) -> Std.Tuple.Prepend -> Std.Println;\n',
            )
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn("1", r.stdout)
            self.assertIn("2", r.stdout)

    def test_cpp_backend_tuple_reverse(self) -> None:
        """Std.Tuple.Reverse reverses a tuple."""
        if shutil.which("c++") is None:
            self.skipTest("c++ compiler is required for cpp backend test")
        with tempfile.TemporaryDirectory() as tmp:
            r = self._run_cpp(
                tmp, "tuple_reverse.fleaux",
                'import Std;\n'
                '((1, 2, 3)) -> Std.Tuple.Reverse -> Std.Println;\n',
            )
            self.assertEqual(r.returncode, 0, r.stderr)
            # should be (3, 2, 1)
            self.assertIn("3", r.stdout)
            self.assertIn("1", r.stdout)

    def test_cpp_backend_tuple_contains(self) -> None:
        """Std.Tuple.Contains returns true/false correctly."""
        if shutil.which("c++") is None:
            self.skipTest("c++ compiler is required for cpp backend test")
        with tempfile.TemporaryDirectory() as tmp:
            r = self._run_cpp(
                tmp, "tuple_contains.fleaux",
                'import Std;\n'
                '((1, 2, 3), 2) -> Std.Tuple.Contains -> Std.Println;\n'
                '((1, 2, 3), 9) -> Std.Tuple.Contains -> Std.Println;\n',
            )
            self.assertEqual(r.returncode, 0, r.stderr)
            lines = [ln for ln in r.stdout.splitlines() if ln.strip()]
            self.assertEqual(lines[0], "True")
            self.assertEqual(lines[1], "False")

    def test_cpp_backend_tuple_zip(self) -> None:
        """Std.Tuple.Zip zips two tuples into a tuple of pairs."""
        if shutil.which("c++") is None:
            self.skipTest("c++ compiler is required for cpp backend test")
        with tempfile.TemporaryDirectory() as tmp:
            r = self._run_cpp(
                tmp, "tuple_zip.fleaux",
                'import Std;\n'
                '((1, 2), (3, 4)) -> Std.Tuple.Zip -> Std.Println;\n',
            )
            self.assertEqual(r.returncode, 0, r.stderr)
            # Expects something like ((1, 3), (2, 4))
            self.assertIn("1", r.stdout)
            self.assertIn("3", r.stdout)

    def test_cpp_backend_tuple_map_user_callable(self) -> None:
        """Std.Tuple.Map applies a user function to every element."""
        if shutil.which("c++") is None:
            self.skipTest("c++ compiler is required for cpp backend test")
        with tempfile.TemporaryDirectory() as tmp:
            r = self._run_cpp(
                tmp, "tuple_map_user_callable.fleaux",
                'import Std;\n'
                'let Double(x: Number): Number = (x, 2) -> Std.Multiply;\n'
                '((1, 2, 3), Double) -> Std.Tuple.Map -> Std.Println;\n',
            )
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn("2 4 6", r.stdout)

    def test_cpp_backend_tuple_map_builtin_callable(self) -> None:
        """Std.Tuple.Map accepts builtin function references as data."""
        if shutil.which("c++") is None:
            self.skipTest("c++ compiler is required for cpp backend test")
        with tempfile.TemporaryDirectory() as tmp:
            r = self._run_cpp(
                tmp, "tuple_map_builtin_callable.fleaux",
                'import Std;\n'
                '((-1, 2, -3), Std.Math.Abs) -> Std.Tuple.Map -> Std.Println;\n',
            )
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn("1 2 3", r.stdout)

    def test_cpp_backend_tuple_filter_user_callable(self) -> None:
        """Std.Tuple.Filter keeps items whose predicate returns true."""
        if shutil.which("c++") is None:
            self.skipTest("c++ compiler is required for cpp backend test")
        with tempfile.TemporaryDirectory() as tmp:
            r = self._run_cpp(
                tmp, "tuple_filter_user_callable.fleaux",
                'import Std;\n'
                'let IsEven(x: Number): Bool = ((x, 2) -> Std.Mod, 0) -> Std.Equal;\n'
                '((1, 2, 3, 4, 5), IsEven) -> Std.Tuple.Filter -> Std.Println;\n',
            )
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn("2 4", r.stdout)

    def test_cpp_backend_tuple_filter_builtin_callable(self) -> None:
        """Std.Tuple.Filter accepts builtin predicate references."""
        if shutil.which("c++") is None:
            self.skipTest("c++ compiler is required for cpp backend test")
        with tempfile.TemporaryDirectory() as tmp:
            r = self._run_cpp(
                tmp, "tuple_filter_builtin_callable.fleaux",
                'import Std;\n'
                '(((1, 0) -> Std.GreaterThan, (0, 1) -> Std.GreaterThan, (3, 2) -> Std.GreaterThan), Std.Not) -> Std.Tuple.Filter -> Std.Println;\n',
            )
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn("False", r.stdout)

    def test_cpp_backend_tuple_map_filter_empty(self) -> None:
        """Std.Tuple.Map and Std.Tuple.Filter preserve empty tuples."""
        if shutil.which("c++") is None:
            self.skipTest("c++ compiler is required for cpp backend test")
        with tempfile.TemporaryDirectory() as tmp:
            r = self._run_cpp(
                tmp, "tuple_map_filter_empty.fleaux",
                'import Std;\n'
                'let Identity(x: Any): Any = x;\n'
                '((), Identity) -> Std.Tuple.Map -> Std.Println;\n'
                '((), Identity) -> Std.Tuple.Filter -> Std.Println;\n',
            )
            self.assertEqual(r.returncode, 0, r.stderr)
            # Println on an empty tuple is equivalent to Python print(*()): blank line.
            self.assertEqual(r.stdout, "\n\n")

    def test_cpp_backend_user_apply_callable_param(self) -> None:
        if shutil.which("c++") is None:
            self.skipTest("c++ compiler is required for cpp backend test")
        with tempfile.TemporaryDirectory() as tmp:
            r = self._run_cpp(
                tmp,
                "user_apply_callable_param.fleaux",
                "import Std;\n"
                "let Double(x: Number): Number = (x, 2) -> Std.Multiply;\n"
                "let MyApply(val: Any, func: Any): Any = (val) -> func;\n"
                "(5, Double) -> MyApply -> Std.Println;\n",
            )
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn("10", r.stdout)

    def test_cpp_backend_nested_callable_argument(self) -> None:
        if shutil.which("c++") is None:
            self.skipTest("c++ compiler is required for cpp backend test")
        with tempfile.TemporaryDirectory() as tmp:
            r = self._run_cpp(
                tmp,
                "nested_callable_argument.fleaux",
                "import Std;\n"
                "let Double(x: Number): Number = (x, 2) -> Std.Multiply;\n"
                "let MyApply(val: Any, func: Any): Any = (val) -> func;\n"
                "let ApplyNested(val: Any, outer: Any, inner: Any): Any = (val, inner) -> outer;\n"
                "(5, MyApply, Double) -> ApplyNested -> Std.Println;\n",
            )
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn("10", r.stdout)

if __name__ == "__main__":
    unittest.main()

