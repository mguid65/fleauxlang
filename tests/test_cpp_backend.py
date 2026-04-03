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
            generated_code = generated.read_text(encoding="utf-8")
            self.assertNotIn("using namespace fleaux::runtime;", generated_code)
            self.assertIn("fleaux::runtime::set_process_args", generated_code)

    def test_cpp_backend_no_run_compiles_without_executing(self) -> None:
        if shutil.which("c++") is None:
            self.skipTest("c++ compiler is required for cpp backend test")
        repo_root = Path(__file__).resolve().parents[1]
        launcher = repo_root / "fleaux"
        with tempfile.TemporaryDirectory() as tmp_dir:
            source = Path(tmp_dir) / "no_run_cpp_only.fleaux"
            source.write_text(
                "import Std;\n"
                "(7) -> Std.Exit;\n",
                encoding="utf-8",
            )
            completed = subprocess.run(
                [str(launcher), str(source), "--backend", "cpp", "--no-run"],
                cwd=tmp_dir,
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(completed.returncode, 0, completed.stderr)
            self.assertTrue((Path(tmp_dir) / "fleaux_generated_module_no_run_cpp_only").exists())

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

    def test_cpp_backend_printf_builtin_more_than_eight_args(self) -> None:
        if shutil.which("c++") is None:
            self.skipTest("c++ compiler is required for cpp backend test")

        repo_root = Path(__file__).resolve().parents[1]
        launcher = repo_root / "fleaux"

        with tempfile.TemporaryDirectory() as tmp_dir:
            source = Path(tmp_dir) / "printf_cpp_backend_many_args_test.fleaux"
            source.write_text(
                "import Std;\n"
                '("{} {} {} {} {} {} {} {} {} {}", 0, 1, 2, 3, 4, 5, 6, 7, 8, 9) -> Std.Printf;\n',
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
            self.assertIn("0 1 2 3 4 5 6 7 8 9", completed.stdout)

    def test_cpp_backend_printf_fallback_semantics(self) -> None:
        if shutil.which("c++") is None:
            self.skipTest("c++ compiler is required for cpp backend test")

        repo_root = Path(__file__).resolve().parents[1]
        launcher = repo_root / "fleaux"

        with tempfile.TemporaryDirectory() as tmp_dir:
            source = Path(tmp_dir) / "printf_cpp_backend_fallback_semantics.fleaux"
            source.write_text(
                "import Std;\n"
                '("{} {} {} {} {} {} {} {} {} {}", 0, 1, 2, 3, 4, 5, 6, 7, 8, 9) -> Std.Printf;\n'
                '("{9}|{0}|{5}", 0, 1, 2, 3, 4, 5, 6, 7, 8, 9) -> Std.Printf;\n'
                '("{{left}}:{}:{{right}}", 0, 1, 2, 3, 4, 5, 6, 7, 8, 9) -> Std.Printf;\n'
                '("{0:>5}|{1:<4}", 9, 2, 3, 4, 5, 6, 7, 8, 9, 10) -> Std.Printf;\n',
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
            lines = [ln for ln in completed.stdout.splitlines() if ln.strip()]
            self.assertIn("0 1 2 3 4 5 6 7 8 9", lines[0])
            self.assertIn("9|0|5", lines[1])
            self.assertIn("{left}:0:{right}", lines[2])
            self.assertIn("    9|2   ", lines[3])

    def test_cpp_backend_printf_fallback_mixed_auto_manual_rejected(self) -> None:
        if shutil.which("c++") is None:
            self.skipTest("c++ compiler is required for cpp backend test")

        repo_root = Path(__file__).resolve().parents[1]
        launcher = repo_root / "fleaux"

        with tempfile.TemporaryDirectory() as tmp_dir:
            source = Path(tmp_dir) / "printf_cpp_backend_mixed_index_error.fleaux"
            source.write_text(
                "import Std;\n"
                '("{} {1}", 0, 1, 2, 3, 4, 5, 6, 7, 8, 9) -> Std.Printf;\n',
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
            self.assertIn("cannot mix automatic and manual field numbering", completed.stderr)

    def test_cpp_backend_printf_fallback_missing_argument_rejected(self) -> None:
        if shutil.which("c++") is None:
            self.skipTest("c++ compiler is required for cpp backend test")

        repo_root = Path(__file__).resolve().parents[1]
        launcher = repo_root / "fleaux"

        with tempfile.TemporaryDirectory() as tmp_dir:
            source = Path(tmp_dir) / "printf_cpp_backend_missing_arg_error.fleaux"
            source.write_text(
                "import Std;\n"
                '("{15}", 0, 1, 2, 3, 4, 5, 6, 7, 8, 9) -> Std.Printf;\n',
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
            self.assertIn("references a missing argument", completed.stderr)

    def test_cpp_backend_printf_fallback_unmatched_brace_rejected(self) -> None:
        if shutil.which("c++") is None:
            self.skipTest("c++ compiler is required for cpp backend test")

        repo_root = Path(__file__).resolve().parents[1]
        launcher = repo_root / "fleaux"

        with tempfile.TemporaryDirectory() as tmp_dir:
            source = Path(tmp_dir) / "printf_cpp_backend_unmatched_brace_error.fleaux"
            source.write_text(
                "import Std;\n"
                '("{", 0, 1, 2, 3, 4, 5, 6, 7, 8, 9) -> Std.Printf;\n',
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
            self.assertIn("unmatched '{'", completed.stderr)

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

    def test_cpp_backend_get_args_builtin_forwards_runtime_args(self) -> None:
        if shutil.which("c++") is None:
            self.skipTest("c++ compiler is required for cpp backend test")

        repo_root = Path(__file__).resolve().parents[1]
        launcher = repo_root / "fleaux"

        with tempfile.TemporaryDirectory() as tmp_dir:
            source = Path(tmp_dir) / "getargs_cpp_backend_forwarded_args_test.fleaux"
            source.write_text(
                "import Std;\n"
                "() -> Std.GetArgs -> Std.Println;\n",
                encoding="utf-8",
            )
            completed = subprocess.run(
                [str(launcher), str(source), "--backend", "cpp", "--", "alpha", "beta"],
                cwd=tmp_dir,
                text=True,
                capture_output=True,
                check=False,
            )

            self.assertEqual(completed.returncode, 0, completed.stderr)
            self.assertIn("alpha", completed.stdout)
            self.assertIn("beta", completed.stdout)

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

    def test_cpp_backend_file_delete_missing_returns_false(self) -> None:
        if shutil.which("c++") is None:
            self.skipTest("c++ compiler is required for cpp backend test")
        with tempfile.TemporaryDirectory() as tmp:
            r = self._run_cpp(
                tmp,
                "file_delete_missing.fleaux",
                'import Std;\n'
                '("missing-file.txt") -> Std.File.Delete -> Std.Println;\n',
            )
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn("False", r.stdout)

    def test_cpp_backend_dir_delete_missing_returns_false(self) -> None:
        if shutil.which("c++") is None:
            self.skipTest("c++ compiler is required for cpp backend test")
        with tempfile.TemporaryDirectory() as tmp:
            r = self._run_cpp(
                tmp,
                "dir_delete_missing.fleaux",
                'import Std;\n'
                '("missing-dir") -> Std.Dir.Delete -> Std.Println;\n',
            )
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn("False", r.stdout)

    def test_cpp_backend_file_delete_non_empty_directory_errors(self) -> None:
        if shutil.which("c++") is None:
            self.skipTest("c++ compiler is required for cpp backend test")
        with tempfile.TemporaryDirectory() as tmp:
            nested = Path(tmp) / "not-a-file"
            nested.mkdir()
            (nested / "child.txt").write_text("x", encoding="utf-8")
            r = self._run_cpp(
                tmp,
                "file_delete_non_empty_dir_error.fleaux",
                'import Std;\n'
                '("not-a-file") -> Std.File.Delete;\n',
            )
            self.assertNotEqual(r.returncode, 0)
            self.assertIn("FileDelete failed", r.stderr)

    # ── OS.Env / OS.HasEnv / OS.SetEnv / OS.UnsetEnv edge cases ─────────────

    def _run_cpp(self, tmp_dir: str, source_name: str, code: str,
                 extra_env: dict | None = None,
                 extra_args: list[str] | None = None) -> subprocess.CompletedProcess:
        """Helper: write *code* to a .fleaux file, compile + run with cpp backend."""
        repo_root = Path(__file__).resolve().parents[1]
        launcher = repo_root / "fleaux"
        source = Path(tmp_dir) / source_name
        source.write_text(code, encoding="utf-8")
        env = dict(os.environ)
        if extra_env:
            env.update(extra_env)
        cmd = [str(launcher), str(source), "--backend", "cpp"]
        if extra_args:
            cmd.extend(extra_args)
        return subprocess.run(
            cmd,
            cwd=tmp_dir,
            text=True,
            capture_output=True,
            check=False,
            env=env,
        )

    def test_cpp_backend_compiler_flag_explicit_cxx(self) -> None:
        """--compiler c++ selects the c++ compiler explicitly."""
        if shutil.which("c++") is None:
            self.skipTest("c++ compiler is required for cpp backend test")
        with tempfile.TemporaryDirectory() as tmp:
            r = self._run_cpp(
                tmp, "compiler_flag_cxx.fleaux",
                'import Std;\n'
                '(1, 2) -> Std.Add -> Std.Println;\n',
                extra_args=["--compiler", "c++"],
            )
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn("3", r.stdout)

    def test_cpp_backend_compiler_flag_absolute_path(self) -> None:
        """--compiler accepts an absolute path to a compiler binary."""
        found = shutil.which("c++")
        if found is None:
            self.skipTest("c++ compiler is required for cpp backend test")
        with tempfile.TemporaryDirectory() as tmp:
            r = self._run_cpp(
                tmp, "compiler_flag_abspath.fleaux",
                'import Std;\n'
                '(10, 5) -> Std.Subtract -> Std.Println;\n',
                extra_args=["--compiler", found],
            )
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn("5", r.stdout)

    def test_cpp_backend_compiler_flag_clangpp(self) -> None:
        """--compiler clang++ selects clang++ if available."""
        if shutil.which("clang++") is None:
            self.skipTest("clang++ not available")
        with tempfile.TemporaryDirectory() as tmp:
            r = self._run_cpp(
                tmp, "compiler_flag_clangpp.fleaux",
                'import Std;\n'
                '(1, 2) -> Std.Add -> Std.Println;\n',
                extra_args=["--compiler", "clang++"],
            )
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn("3", r.stdout)

    def test_cpp_backend_compiler_flag_nonexistent_fails(self) -> None:
        """--compiler with a non-existent binary exits with error code 2."""
        with tempfile.TemporaryDirectory() as tmp:
            r = self._run_cpp(
                tmp, "compiler_flag_bad.fleaux",
                'import Std;\n'
                '(1, 2) -> Std.Add -> Std.Println;\n',
                extra_args=["--compiler", "no_such_compiler_xyzzy"],
            )
            self.assertEqual(r.returncode, 2)

    def test_cpp_backend_compiler_flag_bad_path_fails(self) -> None:
        """--compiler with a non-existent absolute path exits with error code 2."""
        with tempfile.TemporaryDirectory() as tmp:
            r = self._run_cpp(
                tmp, "compiler_flag_badpath.fleaux",
                'import Std;\n'
                '(1, 2) -> Std.Add -> Std.Println;\n',
                extra_args=["--compiler", "/no/such/compiler/clang++"],
            )
            self.assertEqual(r.returncode, 2)



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
            "13_comparison_and_logic.fleaux": ["True", "False"],
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
            self.assertIn("Null", r.stdout)

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

    # ── Path.Join multi-segment tests ─────────────────────────────────────────

    def test_cpp_backend_path_join_two_segments(self) -> None:
        """Path.Join with 2 segments works as before."""
        if shutil.which("c++") is None:
            self.skipTest("c++ compiler is required for cpp backend test")
        with tempfile.TemporaryDirectory() as tmp:
            r = self._run_cpp(
                tmp, "path_join_two.fleaux",
                'import Std;\n'
                '("a", "b") -> Std.Path.Join -> Std.Println;\n',
            )
            self.assertEqual(r.returncode, 0, r.stderr)
            import os as _os
            expected = _os.path.join("a", "b")
            self.assertIn(expected, r.stdout)

    def test_cpp_backend_path_join_three_segments(self) -> None:
        """Path.Join with 3 segments joins all three."""
        if shutil.which("c++") is None:
            self.skipTest("c++ compiler is required for cpp backend test")
        with tempfile.TemporaryDirectory() as tmp:
            r = self._run_cpp(
                tmp, "path_join_three.fleaux",
                'import Std;\n'
                '("a", "b", "c") -> Std.Path.Join -> Std.Println;\n',
            )
            self.assertEqual(r.returncode, 0, r.stderr)
            import os as _os
            expected = _os.path.join("a", "b", "c")
            self.assertIn(expected, r.stdout)

    def test_cpp_backend_path_join_four_segments(self) -> None:
        """Path.Join with 4 segments joins all four."""
        if shutil.which("c++") is None:
            self.skipTest("c++ compiler is required for cpp backend test")
        with tempfile.TemporaryDirectory() as tmp:
            r = self._run_cpp(
                tmp, "path_join_four.fleaux",
                'import Std;\n'
                '("root", "sub", "dir", "file.txt") -> Std.Path.Join -> Std.Println;\n',
            )
            self.assertEqual(r.returncode, 0, r.stderr)
            import os as _os
            expected = _os.path.join("root", "sub", "dir", "file.txt")
            self.assertIn(expected, r.stdout)

    def test_cpp_backend_path_join_absolute_mid_segment_overrides(self) -> None:
        """Joining an absolute segment mid-way resets the path (POSIX semantics)."""
        if shutil.which("c++") is None:
            self.skipTest("c++ compiler is required for cpp backend test")
        import sys as _sys
        if _sys.platform == "win32":
            self.skipTest("POSIX-only semantics test")
        with tempfile.TemporaryDirectory() as tmp:
            r = self._run_cpp(
                tmp, "path_join_absolute.fleaux",
                'import Std;\n'
                '("a", "/abs", "c") -> Std.Path.Join -> Std.Println;\n',
            )
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn("/abs/c", r.stdout)

    def test_cpp_backend_path_join_one_segment_raises(self) -> None:
        """Path.Join with fewer than 2 segments raises a runtime error."""
        if shutil.which("c++") is None:
            self.skipTest("c++ compiler is required for cpp backend test")
        with tempfile.TemporaryDirectory() as tmp:
            r = self._run_cpp(
                tmp, "path_join_one.fleaux",
                'import Std;\n'
                '("only",) -> Std.Path.Join -> Std.Println;\n',
            )
            self.assertNotEqual(r.returncode, 0)

    # ── Streaming file handle builtins ────────────────────────────────────────

    def test_cpp_backend_file_open_readline_close(self) -> None:
        """File.Open / File.ReadLine / File.Close stream lines one-by-one."""
        if shutil.which("c++") is None:
            self.skipTest("c++ compiler is required for cpp backend test")
        with tempfile.TemporaryDirectory() as tmp:
            data_file = Path(tmp) / "data.txt"
            data_file.write_text("alpha\nbeta\ngamma\n", encoding="utf-8")
            r = self._run_cpp(
                tmp, "file_readline.fleaux",
                'import Std;\n'
                'let PrintLineGetHandle(h: Any, line: String): Any =\n'
                '    (line -> Std.Println, h) -> (_, 1) -> Std.ElementAt;\n'
                'let Continue(h: Any, line: String, eof: Bool): Bool = (eof) -> Std.Not;\n'
                'let Step(h: Any, line: String, eof: Bool): Tuple(Any, String, Bool) =\n'
                '    (h, line) -> PrintLineGetHandle -> Std.File.ReadLine;\n'
                'let ReadFile(path: String): Any =\n'
                '    ((path, "r") -> Std.File.Open -> Std.File.ReadLine, Continue, Step) -> Std.Loop;\n'
                '("data.txt") -> ReadFile;\n',
            )
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn("alpha", r.stdout)
            self.assertIn("beta", r.stdout)
            self.assertIn("gamma", r.stdout)
            self.assertNotIn("__fleaux_handle__", r.stdout)

    def test_cpp_backend_file_close_is_idempotent(self) -> None:
        """File.Close on an already-closed handle returns False without throwing."""
        if shutil.which("c++") is None:
            self.skipTest("c++ compiler is required for cpp backend test")
        with tempfile.TemporaryDirectory() as tmp:
            data_file = Path(tmp) / "close_test.txt"
            data_file.write_text("x\n", encoding="utf-8")
            r = self._run_cpp(
                tmp, "file_close_idempotent.fleaux",
                'import Std;\n'
                '("close_test.txt", "r") -> Std.File.Open -> Std.File.Close -> Std.Println;\n',
            )
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn("True", r.stdout)

    def test_cpp_backend_file_writechunk_and_readback(self) -> None:
        """File.WriteChunk writes data that can be read back with ReadText."""
        if shutil.which("c++") is None:
            self.skipTest("c++ compiler is required for cpp backend test")
        with tempfile.TemporaryDirectory() as tmp:
            r = self._run_cpp(
                tmp, "file_writechunk.fleaux",
                'import Std;\n'
                '("chunk_out.txt", "w") -> Std.File.Open\n'
                '    -> (_, "hello from chunk") -> Std.File.WriteChunk\n'
                '    -> Std.File.Close;\n'
                '("chunk_out.txt") -> Std.File.ReadText -> Std.Println;\n',
            )
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn("hello from chunk", r.stdout)

    def test_cpp_backend_file_withopen_guarantees_close(self) -> None:
        """File.WithOpen calls func and returns its result."""
        if shutil.which("c++") is None:
            self.skipTest("c++ compiler is required for cpp backend test")
        with tempfile.TemporaryDirectory() as tmp:
            data_file = Path(tmp) / "withopen.txt"
            data_file.write_text("scoped\n", encoding="utf-8")
            r = self._run_cpp(
                tmp, "file_withopen.fleaux",
                'import Std;\n'
                'let ReadFirst(h: Any): String =\n'
                '    (h) -> Std.File.ReadLine -> (_, 1) -> Std.ElementAt;\n'
                '("withopen.txt", "r", ReadFirst) -> Std.File.WithOpen -> Std.Println;\n',
            )
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn("scoped", r.stdout)

    def test_cpp_backend_file_open_missing_raises(self) -> None:
        """File.Open on a non-existent file raises a runtime error."""
        if shutil.which("c++") is None:
            self.skipTest("c++ compiler is required for cpp backend test")
        with tempfile.TemporaryDirectory() as tmp:
            r = self._run_cpp(
                tmp, "file_open_missing.fleaux",
                'import Std;\n'
                '("no_such_file_xyzzy.txt", "r") -> Std.File.Open -> Std.Println;\n',
            )
            self.assertNotEqual(r.returncode, 0)

    def test_cpp_backend_file_open_wrong_arity_raises(self) -> None:
        """File.Open rejects arity other than 1 or 2 arguments."""
        if shutil.which("c++") is None:
            self.skipTest("c++ compiler is required for cpp backend test")
        with tempfile.TemporaryDirectory() as tmp:
            data_file = Path(tmp) / "ok.txt"
            data_file.write_text("x\n", encoding="utf-8")
            r = self._run_cpp(
                tmp, "file_open_wrong_arity.fleaux",
                'import Std;\n'
                '("ok.txt", "r", "extra") -> Std.File.Open -> Std.Println;\n',
            )
            self.assertNotEqual(r.returncode, 0)
            self.assertIn("FileOpen: expected (path,) or (path, mode)", r.stderr)

    def test_cpp_backend_file_readline_on_write_only_handle_raises(self) -> None:
        """File.ReadLine raises a read-failure error on non-readable handles."""
        if shutil.which("c++") is None:
            self.skipTest("c++ compiler is required for cpp backend test")
        with tempfile.TemporaryDirectory() as tmp:
            r = self._run_cpp(
                tmp, "file_readline_write_only_error.fleaux",
                'import Std;\n'
                '("write_only.txt", "w") -> Std.File.Open -> Std.File.ReadLine -> Std.Println;\n',
            )
            self.assertNotEqual(r.returncode, 0)
            self.assertIn("FileReadLine: read failed", r.stderr)


if __name__ == "__main__":
    unittest.main()

