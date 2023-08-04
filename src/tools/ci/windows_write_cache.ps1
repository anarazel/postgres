Get-ItemProperty -path "HKLM:/SYSTEM/CurrentControlSet/Enum/SCSI/*/*/Device Parameters/Disk" -name CacheIsPowerProtected
Set-ItemProperty -path "HKLM:/SYSTEM/CurrentControlSet/Enum/SCSI/*/*/Device Parameters/Disk" -name CacheIsPowerProtected -Value 0
Get-ItemProperty -path "HKLM:/SYSTEM/CurrentControlSet/Enum/SCSI/*/*/Device Parameters/Disk" -name CacheIsPowerProtectedy
