# Dumper-7 DLL を Foxhole プロセスに注入するスクリプト
# 使い方: Foxholeを起動した後、管理者権限のPowerShellでこのスクリプトを実行

# Foxhole プロセスを探す
$proc = Get-Process "War-Win64-Shipping" -ErrorAction SilentlyContinue
if (-not $proc) {
    Write-Host "Foxhole (War-Win64-Shipping.exe) が見つかりません。先にゲームを起動してください。" -ForegroundColor Red
    exit 1
}

$procId = $proc.Id
Write-Host "Foxhole プロセス発見: PID=$procId" -ForegroundColor Green

$dumperDll = "C:\Program Files (x86)\Steam\steamapps\common\Foxhole\Mods\Dumper-7\build\bin\Release\Dumper-7.dll"
if (-not (Test-Path $dumperDll)) {
    Write-Host "Dumper-7.dll が見つかりません: $dumperDll" -ForegroundColor Red
    exit 1
}

Write-Host "Dumper-7.dll をプロセスに注入中..."
Write-Host "  DLL: $dumperDll"

# C# DLL Injector (CreateRemoteThread + LoadLibraryA)
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
using System.Text;
using System.Diagnostics;

public class DllInjector {
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern IntPtr OpenProcess(uint access, bool inherit, int pid);

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern IntPtr VirtualAllocEx(IntPtr hProcess, IntPtr lpAddress, uint dwSize, uint flAllocationType, uint flProtect);

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool WriteProcessMemory(IntPtr hProcess, IntPtr lpBaseAddress, byte[] lpBuffer, uint nSize, out IntPtr lpNumberOfBytesWritten);

    [DllImport("kernel32.dll")]
    static extern IntPtr GetProcAddress(IntPtr hModule, string procName);

    [DllImport("kernel32.dll")]
    static extern IntPtr GetModuleHandle(string lpModuleName);

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern IntPtr CreateRemoteThread(IntPtr hProcess, IntPtr lpThreadAttributes, uint dwStackSize, IntPtr lpStartAddress, IntPtr lpParameter, uint dwCreationFlags, out IntPtr lpThreadId);

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern uint WaitForSingleObject(IntPtr hHandle, uint dwMilliseconds);

    [DllImport("kernel32.dll")]
    static extern bool CloseHandle(IntPtr hObject);

    [DllImport("kernel32.dll")]
    static extern bool VirtualFreeEx(IntPtr hProcess, IntPtr lpAddress, uint dwSize, uint dwFreeType);

    const uint PROCESS_ALL_ACCESS = 0x1F0FFF;
    const uint MEM_COMMIT = 0x1000;
    const uint MEM_RESERVE = 0x2000;
    const uint PAGE_READWRITE = 0x04;
    const uint MEM_RELEASE = 0x8000;

    public static bool Inject(int pid, string dllPath) {
        IntPtr hProcess = OpenProcess(PROCESS_ALL_ACCESS, false, pid);
        if (hProcess == IntPtr.Zero) {
            Console.WriteLine("OpenProcess failed: " + Marshal.GetLastWin32Error());
            return false;
        }

        byte[] dllBytes = Encoding.ASCII.GetBytes(dllPath + "\0");
        IntPtr remoteMem = VirtualAllocEx(hProcess, IntPtr.Zero, (uint)dllBytes.Length, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (remoteMem == IntPtr.Zero) {
            Console.WriteLine("VirtualAllocEx failed");
            CloseHandle(hProcess);
            return false;
        }

        IntPtr written;
        WriteProcessMemory(hProcess, remoteMem, dllBytes, (uint)dllBytes.Length, out written);

        IntPtr loadLib = GetProcAddress(GetModuleHandle("kernel32.dll"), "LoadLibraryA");
        IntPtr threadId;
        IntPtr hThread = CreateRemoteThread(hProcess, IntPtr.Zero, 0, loadLib, remoteMem, 0, out threadId);
        if (hThread == IntPtr.Zero) {
            Console.WriteLine("CreateRemoteThread failed: " + Marshal.GetLastWin32Error());
            VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
            CloseHandle(hProcess);
            return false;
        }

        WaitForSingleObject(hThread, 30000);
        VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
        CloseHandle(hThread);
        CloseHandle(hProcess);
        return true;
    }
}
"@

$result = [DllInjector]::Inject($procId, $dumperDll)
if ($result) {
    Write-Host "注入成功! SDKは C:\Dumper-7 に生成されます。" -ForegroundColor Green
    Write-Host "コンソールウィンドウに進捗が表示されます。完了まで待ってください。" -ForegroundColor Yellow
    
    # SDK生成完了を待つ
    Write-Host "`nSDK生成完了を待機中..."
    $timeout = 120
    $waited = 0
    while ($waited -lt $timeout) {
        Start-Sleep -Seconds 5
        $waited += 5
        if (Test-Path "C:\Dumper-7\SDK") {
            $files = Get-ChildItem "C:\Dumper-7\SDK" -File -ErrorAction SilentlyContinue
            if ($files.Count -gt 0) {
                Write-Host "`nSDK生成完了! ($($files.Count) ファイル)" -ForegroundColor Green
                Get-ChildItem "C:\Dumper-7\SDK" | Select-Object Name, Length | Format-Table -AutoSize
                break
            }
        }
        Write-Host "  $waited 秒経過..."
    }
} else {
    Write-Host "注入失敗" -ForegroundColor Red
}
