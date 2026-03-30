import fleaux_std_builtins as fstd

def _fleaux_missing_builtin(name):
    class _MissingBuiltin:
        def __ror__(self, _tuple_args):
            raise NotImplementedError(
                f"Builtin '{name}' is not yet implemented in fleaux_std_builtins.py"
            )
    return _MissingBuiltin

def _fleaux_impl_Std_Half(*_fleaux_args):
    pass
    return 0.05
Std_Half = fstd.make_node(_fleaux_impl_Std_Half)

def _fleaux_impl_Std_Third(*_fleaux_args):
    pass
    return 0.3333333333333333
Std_Third = fstd.make_node(_fleaux_impl_Std_Third)

def _fleaux_impl_Std_TwoThirds(*_fleaux_args):
    pass
    return 0.6666666666666666
Std_TwoThirds = fstd.make_node(_fleaux_impl_Std_TwoThirds)

def _fleaux_impl_Std_Sixth(*_fleaux_args):
    pass
    return 0.16666666666666666
Std_Sixth = fstd.make_node(_fleaux_impl_Std_Sixth)

def _fleaux_impl_Std_ThreeQuarters(*_fleaux_args):
    pass
    return 0.75
Std_ThreeQuarters = fstd.make_node(_fleaux_impl_Std_ThreeQuarters)

def _fleaux_impl_Std_RootTwo(*_fleaux_args):
    pass
    return 1.4142135623730951
Std_RootTwo = fstd.make_node(_fleaux_impl_Std_RootTwo)

def _fleaux_impl_Std_RootThree(*_fleaux_args):
    pass
    return 1.7320508075688772
Std_RootThree = fstd.make_node(_fleaux_impl_Std_RootThree)

def _fleaux_impl_Std_HalfRootTwo(*_fleaux_args):
    pass
    return 0.7071067811865476
Std_HalfRootTwo = fstd.make_node(_fleaux_impl_Std_HalfRootTwo)

def _fleaux_impl_Std_LnTwo(*_fleaux_args):
    pass
    return 0.6931471805599453
Std_LnTwo = fstd.make_node(_fleaux_impl_Std_LnTwo)

def _fleaux_impl_Std_LnLnTwo(*_fleaux_args):
    pass
    return -0.36651292058166435
Std_LnLnTwo = fstd.make_node(_fleaux_impl_Std_LnLnTwo)

def _fleaux_impl_Std_RootLnFour(*_fleaux_args):
    pass
    return 1.1774100225154747
Std_RootLnFour = fstd.make_node(_fleaux_impl_Std_RootLnFour)

def _fleaux_impl_Std_OneDivRootTwo(*_fleaux_args):
    pass
    return 0.7071067811865476
Std_OneDivRootTwo = fstd.make_node(_fleaux_impl_Std_OneDivRootTwo)

def _fleaux_impl_Std_Pi(*_fleaux_args):
    pass
    return 3.141592653589793
Std_Pi = fstd.make_node(_fleaux_impl_Std_Pi)

def _fleaux_impl_Std_HalfPi(*_fleaux_args):
    pass
    return 1.5707963267948966
Std_HalfPi = fstd.make_node(_fleaux_impl_Std_HalfPi)

def _fleaux_impl_Std_ThirdPi(*_fleaux_args):
    pass
    return 1.0471975511965979
Std_ThirdPi = fstd.make_node(_fleaux_impl_Std_ThirdPi)

def _fleaux_impl_Std_SixthPi(*_fleaux_args):
    pass
    return 0.5235987755982989
Std_SixthPi = fstd.make_node(_fleaux_impl_Std_SixthPi)

def _fleaux_impl_Std_TwoPi(*_fleaux_args):
    pass
    return 6.283185307179586
Std_TwoPi = fstd.make_node(_fleaux_impl_Std_TwoPi)

def _fleaux_impl_Std_Tau(*_fleaux_args):
    pass
    return 6.283185307179586
Std_Tau = fstd.make_node(_fleaux_impl_Std_Tau)

def _fleaux_impl_Std_TwoThirdsPi(*_fleaux_args):
    pass
    return 2.0943951023931957
Std_TwoThirdsPi = fstd.make_node(_fleaux_impl_Std_TwoThirdsPi)

def _fleaux_impl_Std_ThreeQuartersPi(*_fleaux_args):
    pass
    return 2.356194490192345
Std_ThreeQuartersPi = fstd.make_node(_fleaux_impl_Std_ThreeQuartersPi)

def _fleaux_impl_Std_FourThirdsPi(*_fleaux_args):
    pass
    return 4.188790204786391
Std_FourThirdsPi = fstd.make_node(_fleaux_impl_Std_FourThirdsPi)

def _fleaux_impl_Std_OneDivTwoPi(*_fleaux_args):
    pass
    return 0.15915494309189535
Std_OneDivTwoPi = fstd.make_node(_fleaux_impl_Std_OneDivTwoPi)

def _fleaux_impl_Std_OneDivRootTwoPi(*_fleaux_args):
    pass
    return 0.3989422804014327
Std_OneDivRootTwoPi = fstd.make_node(_fleaux_impl_Std_OneDivRootTwoPi)

def _fleaux_impl_Std_RootPi(*_fleaux_args):
    pass
    return 1.772453850905516
Std_RootPi = fstd.make_node(_fleaux_impl_Std_RootPi)

def _fleaux_impl_Std_RootHalfPi(*_fleaux_args):
    pass
    return 1.2533141373155003
Std_RootHalfPi = fstd.make_node(_fleaux_impl_Std_RootHalfPi)

def _fleaux_impl_Std_RootTwoPi(*_fleaux_args):
    pass
    return 2.5066282746310007
Std_RootTwoPi = fstd.make_node(_fleaux_impl_Std_RootTwoPi)

def _fleaux_impl_Std_LogRootTwoPi(*_fleaux_args):
    pass
    return 0.9189385332046728
Std_LogRootTwoPi = fstd.make_node(_fleaux_impl_Std_LogRootTwoPi)

def _fleaux_impl_Std_OneDivRootPi(*_fleaux_args):
    pass
    return 0.5641895835477563
Std_OneDivRootPi = fstd.make_node(_fleaux_impl_Std_OneDivRootPi)

def _fleaux_impl_Std_RootOneDivPi(*_fleaux_args):
    pass
    return 0.5641895835477563
Std_RootOneDivPi = fstd.make_node(_fleaux_impl_Std_RootOneDivPi)

def _fleaux_impl_Std_PiMinusThree(*_fleaux_args):
    pass
    return 0.14159265358979323
Std_PiMinusThree = fstd.make_node(_fleaux_impl_Std_PiMinusThree)

def _fleaux_impl_Std_FourMinusPi(*_fleaux_args):
    pass
    return 0.8584073464102068
Std_FourMinusPi = fstd.make_node(_fleaux_impl_Std_FourMinusPi)

def _fleaux_impl_Std_PiPowE(*_fleaux_args):
    pass
    return 22.459157718361045
Std_PiPowE = fstd.make_node(_fleaux_impl_Std_PiPowE)

def _fleaux_impl_Std_PiSqr(*_fleaux_args):
    pass
    return 9.869604401089358
Std_PiSqr = fstd.make_node(_fleaux_impl_Std_PiSqr)

def _fleaux_impl_Std_PiSqrDivSix(*_fleaux_args):
    pass
    return 1.6449340668482264
Std_PiSqrDivSix = fstd.make_node(_fleaux_impl_Std_PiSqrDivSix)

def _fleaux_impl_Std_PiCubed(*_fleaux_args):
    pass
    return 31.00627668029982
Std_PiCubed = fstd.make_node(_fleaux_impl_Std_PiCubed)

def _fleaux_impl_Std_CbrtPi(*_fleaux_args):
    pass
    return 1.4645918875615234
Std_CbrtPi = fstd.make_node(_fleaux_impl_Std_CbrtPi)

def _fleaux_impl_Std_OneDivCbrtPi(*_fleaux_args):
    pass
    return 0.6827840632552957
Std_OneDivCbrtPi = fstd.make_node(_fleaux_impl_Std_OneDivCbrtPi)

def _fleaux_impl_Std_Log2E(*_fleaux_args):
    pass
    return 1.4426950408889634
Std_Log2E = fstd.make_node(_fleaux_impl_Std_Log2E)

def _fleaux_impl_Std_E(*_fleaux_args):
    pass
    return 2.718281828459045
Std_E = fstd.make_node(_fleaux_impl_Std_E)

def _fleaux_impl_Std_ExpMinusHalf(*_fleaux_args):
    pass
    return 0.6065306597126334
Std_ExpMinusHalf = fstd.make_node(_fleaux_impl_Std_ExpMinusHalf)

def _fleaux_impl_Std_ExpMinusOne(*_fleaux_args):
    pass
    return 0.36787944117144233
Std_ExpMinusOne = fstd.make_node(_fleaux_impl_Std_ExpMinusOne)

def _fleaux_impl_Std_EPowPi(*_fleaux_args):
    pass
    return 23.14069263277927
Std_EPowPi = fstd.make_node(_fleaux_impl_Std_EPowPi)

def _fleaux_impl_Std_RootE(*_fleaux_args):
    pass
    return 1.6487212707001282
Std_RootE = fstd.make_node(_fleaux_impl_Std_RootE)

def _fleaux_impl_Std_Log10E(*_fleaux_args):
    pass
    return 0.4342944819032518
Std_Log10E = fstd.make_node(_fleaux_impl_Std_Log10E)

def _fleaux_impl_Std_OneDivLog10E(*_fleaux_args):
    pass
    return 2.302585092994046
Std_OneDivLog10E = fstd.make_node(_fleaux_impl_Std_OneDivLog10E)

def _fleaux_impl_Std_LnTen(*_fleaux_args):
    pass
    return 2.302585092994046
Std_LnTen = fstd.make_node(_fleaux_impl_Std_LnTen)

def _fleaux_impl_Std_Degree(*_fleaux_args):
    pass
    return 0.017453292519943295
Std_Degree = fstd.make_node(_fleaux_impl_Std_Degree)

def _fleaux_impl_Std_Radian(*_fleaux_args):
    pass
    return 57.29577951308232
Std_Radian = fstd.make_node(_fleaux_impl_Std_Radian)

def _fleaux_impl_Std_SinOne(*_fleaux_args):
    pass
    return 0.8414709848078965
Std_SinOne = fstd.make_node(_fleaux_impl_Std_SinOne)

def _fleaux_impl_Std_CosOne(*_fleaux_args):
    pass
    return 0.5403023058681398
Std_CosOne = fstd.make_node(_fleaux_impl_Std_CosOne)

def _fleaux_impl_Std_SinhOne(*_fleaux_args):
    pass
    return 1.1752011936438014
Std_SinhOne = fstd.make_node(_fleaux_impl_Std_SinhOne)

def _fleaux_impl_Std_CoshOne(*_fleaux_args):
    pass
    return 1.5430806348152437
Std_CoshOne = fstd.make_node(_fleaux_impl_Std_CoshOne)

def _fleaux_impl_Std_Phi(*_fleaux_args):
    pass
    return 1.618033988749895
Std_Phi = fstd.make_node(_fleaux_impl_Std_Phi)

def _fleaux_impl_Std_LnPhi(*_fleaux_args):
    pass
    return 0.48121182505960347
Std_LnPhi = fstd.make_node(_fleaux_impl_Std_LnPhi)

def _fleaux_impl_Std_OneDivLnPhi(*_fleaux_args):
    pass
    return 2.0780869212350277
Std_OneDivLnPhi = fstd.make_node(_fleaux_impl_Std_OneDivLnPhi)

def _fleaux_impl_Std_Euler(*_fleaux_args):
    pass
    return 0.5772156649015329
Std_Euler = fstd.make_node(_fleaux_impl_Std_Euler)

def _fleaux_impl_Std_OneDivEuler(*_fleaux_args):
    pass
    return 1.7324547146006335
Std_OneDivEuler = fstd.make_node(_fleaux_impl_Std_OneDivEuler)

def _fleaux_impl_Std_EulerSqr(*_fleaux_args):
    pass
    return 0.33317792380771866
Std_EulerSqr = fstd.make_node(_fleaux_impl_Std_EulerSqr)

def _fleaux_impl_Std_ZetaTwo(*_fleaux_args):
    pass
    return 1.6449340668482264
Std_ZetaTwo = fstd.make_node(_fleaux_impl_Std_ZetaTwo)

def _fleaux_impl_Std_ZetaThree(*_fleaux_args):
    pass
    return 1.2020569031595942
Std_ZetaThree = fstd.make_node(_fleaux_impl_Std_ZetaThree)

def _fleaux_impl_Std_Catalan(*_fleaux_args):
    pass
    return 0.915965594177219
Std_Catalan = fstd.make_node(_fleaux_impl_Std_Catalan)

def _fleaux_impl_Std_Glaisher(*_fleaux_args):
    pass
    return 1.2824271291006226
Std_Glaisher = fstd.make_node(_fleaux_impl_Std_Glaisher)

def _fleaux_impl_Std_Khinchin(*_fleaux_args):
    pass
    return 2.6854520010653062
Std_Khinchin = fstd.make_node(_fleaux_impl_Std_Khinchin)

def _fleaux_impl_Std_ExtremeValueSkewness(*_fleaux_args):
    pass
    return 1.1395470994046486
Std_ExtremeValueSkewness = fstd.make_node(_fleaux_impl_Std_ExtremeValueSkewness)

def _fleaux_impl_Std_RayleighSkewness(*_fleaux_args):
    pass
    return 0.6311106578189372
Std_RayleighSkewness = fstd.make_node(_fleaux_impl_Std_RayleighSkewness)

def _fleaux_impl_Std_RayleighKurtosis(*_fleaux_args):
    pass
    return 3.245089300687638
Std_RayleighKurtosis = fstd.make_node(_fleaux_impl_Std_RayleighKurtosis)

def _fleaux_impl_Std_RayleighKurtosisExcess(*_fleaux_args):
    pass
    return 0.24508930068763807
Std_RayleighKurtosisExcess = fstd.make_node(_fleaux_impl_Std_RayleighKurtosisExcess)

def _fleaux_impl_Std_TwoDivPi(*_fleaux_args):
    pass
    return 0.6366197723675814
Std_TwoDivPi = fstd.make_node(_fleaux_impl_Std_TwoDivPi)

def _fleaux_impl_Std_RootTwoDivPi(*_fleaux_args):
    pass
    return 0.7978845608028654
Std_RootTwoDivPi = fstd.make_node(_fleaux_impl_Std_RootTwoDivPi)

def _fleaux_impl_Std_QuarterPi(*_fleaux_args):
    pass
    return 0.7853981633974483
Std_QuarterPi = fstd.make_node(_fleaux_impl_Std_QuarterPi)

def _fleaux_impl_Std_InvPi(*_fleaux_args):
    pass
    return 0.3183098861837907
Std_InvPi = fstd.make_node(_fleaux_impl_Std_InvPi)

def _fleaux_impl_Std_TwoDivRootPi(*_fleaux_args):
    pass
    return 1.1283791670955126
Std_TwoDivRootPi = fstd.make_node(_fleaux_impl_Std_TwoDivRootPi)

Std_Println = fstd.Println

Std_Printf = fstd.Printf

Std_In = fstd.In

Std_Input = fstd.Input

Std_Exit = fstd.Exit

Std_Add = fstd.Add

Std_Subtract = fstd.Subtract

Std_Multiply = fstd.Multiply

Std_Divide = fstd.Divide

Std_Mod = fstd.Mod

Std_Pow = fstd.Pow

Std_Sqrt = fstd.Sqrt

Std_Tan = fstd.Tan

Std_Sin = fstd.Sin

Std_Cos = fstd.Cos

Std_GreaterThan = fstd.GreaterThan

Std_LessThan = fstd.LessThan

Std_GreaterOrEqual = fstd.GreaterOrEqual

Std_LessOrEqual = fstd.LessOrEqual

Std_Equal = fstd.Equal

Std_NotEqual = fstd.NotEqual

Std_Not = fstd.Not

Std_And = fstd.And

Std_Or = fstd.Or

Std_Select = fstd.Select

Std_Apply = fstd.Apply

Std_Branch = fstd.Branch

Std_Loop = fstd.Loop

Std_LoopN = fstd.LoopN

Std_UnaryPlus = fstd.UnaryPlus

Std_UnaryMinus = fstd.UnaryMinus

Std_ToString = fstd.ToString

Std_Math_Sqrt = fstd.Sqrt

Std_Math_Sin = fstd.Sin

Std_Math_Cos = fstd.Cos

Std_Math_Tan = fstd.Tan

Std_Math_Floor = fstd.MathFloor

Std_Math_Ceil = fstd.MathCeil

Std_Math_Abs = fstd.MathAbs

Std_Math_Log = fstd.MathLog

Std_Math_Clamp = fstd.MathClamp

Std_String_Upper = fstd.StringUpper

Std_String_Lower = fstd.StringLower

Std_String_Trim = fstd.StringTrim

Std_String_TrimStart = fstd.StringTrimStart

Std_String_TrimEnd = fstd.StringTrimEnd

Std_String_Split = fstd.StringSplit

Std_String_Join = fstd.StringJoin

Std_String_Replace = fstd.StringReplace

Std_String_Contains = fstd.StringContains

Std_String_StartsWith = fstd.StringStartsWith

Std_String_EndsWith = fstd.StringEndsWith

Std_String_Length = fstd.StringLength

Std_OS_Cwd = fstd.Cwd

Std_OS_Home = fstd.OSHome

Std_OS_TempDir = fstd.OSTempDir

Std_OS_Env = fstd.OSEnv

Std_OS_HasEnv = fstd.OSHasEnv

Std_OS_SetEnv = fstd.OSSetEnv

Std_OS_UnsetEnv = fstd.OSUnsetEnv

Std_OS_IsWindows = fstd.OSIsWindows

Std_OS_IsLinux = fstd.OSIsLinux

Std_OS_IsMacOS = fstd.OSIsMacOS

Std_Path_Join = fstd.PathJoin

Std_Path_Normalize = fstd.PathNormalize

Std_Path_Basename = fstd.PathBasename

Std_Path_Dirname = fstd.PathDirname

Std_Path_Exists = fstd.PathExists

Std_Path_IsFile = fstd.PathIsFile

Std_Path_IsDir = fstd.PathIsDir

Std_Path_Absolute = fstd.PathAbsolute

Std_Path_Extension = fstd.PathExtension

Std_Path_Stem = fstd.PathStem

Std_Path_WithExtension = fstd.PathWithExtension

Std_Path_WithBasename = fstd.PathWithBasename

Std_File_ReadText = fstd.FileReadText

Std_File_WriteText = fstd.FileWriteText

Std_File_AppendText = fstd.FileAppendText

Std_File_ReadLines = fstd.FileReadLines

Std_File_Delete = fstd.FileDelete

Std_File_Size = fstd.FileSize

Std_Dir_Create = fstd.DirCreate

Std_Dir_Delete = fstd.DirDelete

Std_Dir_List = fstd.DirList

Std_Dir_ListFull = fstd.DirListFull

Std_Tuple_Append = fstd.TupleAppend

Std_Tuple_Prepend = fstd.TuplePrepend

Std_Tuple_Reverse = fstd.TupleReverse

Std_Tuple_Contains = fstd.TupleContains

Std_Tuple_Zip = fstd.TupleZip

Std_Tuple_Map = fstd.TupleMap

Std_Tuple_Filter = fstd.TupleFilter

Std_Take = fstd.Take

Std_Drop = fstd.Drop

Std_ElementAt = fstd.ElementAt

Std_Length = fstd.Length

Std_Slice = fstd.Slice

Std_ToNum = fstd.ToNum

Std_GetArgs = fstd.GetArgs
