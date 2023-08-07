# Define the write cache to be power protected. This reduces the rate of cache
# flushes, which seems to help metadata heavy workloads on NTFS. We're just
# testing here anyway, so ...
#
# Let's do so for all disks, this could be useful beyond cirrus-ci.

Set-Location "HKLM:/SYSTEM/CurrentControlSet/Enum/SCSI";

Get-ChildItem -Path "*/*" | foreach-object {
    Push-Location;
    cd /$_;
    pwd;
    cd 'Device Parameters';
    if (!(Test-Path -Path "Disk")) {
	New-Item -Path "Disk";
    }

    Set-ItemProperty -Path Disk -Type DWord -name CacheIsPowerProtected -Value 1;
    Pop-Location;
}
