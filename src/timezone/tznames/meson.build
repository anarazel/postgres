tznames = files(
  'Africa.txt',
  'America.txt',
  'Antarctica.txt',
  'Asia.txt',
  'Atlantic.txt',
  'Australia.txt',
  'Etc.txt',
  'Europe.txt',
  'Indian.txt',
  'Pacific.txt',
)

tznames_sets = files(
  'Default',
  'Australia',
  'India')

install_data(tznames, install_dir: get_option('datadir') / 'timezonesets')
install_data(tznames_sets, install_dir: get_option('datadir') / 'timezonesets')
