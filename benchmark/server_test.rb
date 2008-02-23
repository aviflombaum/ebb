$: << File.expand_path(File.dirname(__FILE__) + '/..')

require 'rubygems'
require 'rack'
require 'application'

module Bytes
  def bytes
    self
  end
  alias :byte :bytes

  def kilobytes
    self * 1024
  end
  alias :kilobyte :kilobytes

  def megabytes
    self * 1024.kilobytes
  end
  alias :megabyte :megabytes

  def gigabytes
    self * 1024.megabytes 
  end
  alias :gigabyte :gigabytes

  def terabytes
    self * 1024.gigabytes
  end
  alias :terabyte :terabytes
  
  def petabytes
    self * 1024.terabytes
  end
  alias :petabyte :petabytes
  
  def exabytes
    self * 1024.petabytes
  end
  alias :exabyte :exabytes
  
end
class Fixnum
  include Bytes
end

def number_to_human_size(size, precision=1)
  size = Kernel.Float(size)
  case
    when size.to_i == 1;    "1 Byte"
    when size < 1.kilobyte; "%d Bytes" % size
    when size < 1.megabyte; "%.#{precision}f KB"  % (size / 1.0.kilobyte)
    when size < 1.gigabyte; "%.#{precision}f MB"  % (size / 1.0.megabyte)
    when size < 1.terabyte; "%.#{precision}f GB"  % (size / 1.0.gigabyte)
    else                    "%.#{precision}f TB"  % (size / 1.0.terabyte)
  end.sub(/([0-9])\.?0+ /, '\1 ' )
rescue
  nil
end

class Array
  def avg
    sum.to_f / length
  end
  
  def sum
    inject(0) { |i, s| s += i }
  end
  
  def rand_each(&block)
    sort_by{ rand }.each &block
  end
end

class ServerTestResults
  def self.open(filename)
    if File.readable?(filename)
      new(Marshal.load(File.read(filename))) 
    else
      new
    end
  end

  def initialize(results = [])
    @results = results
  end
  
  def write(filename='results.dump')
    puts "writing dump file to #{filename}"
    File.open(filename, 'w+') do |f|
      f.write Marshal.dump(@results)
    end
  end
  
  def <<(r)
    @results << r
  end
  
  def length
    @results.length
  end

  def servers
    @results.map {|r| r[:server] }.uniq.sort
  end

  def data(server, what=:size)
    server_data = @results.find_all { |r| r[:server] == server }
    ticks = server_data.map { |d| d[what] }.uniq
    datas = []
    ticks.each do |c|
      measurements = server_data.find_all { |d| d[what] == c }.map { |d| d[:rps] }
      datas << [c, measurements.avg]
    end
    datas
  end

end

class ServerTest
  attr_reader :name, :port, :app, :pid
  def initialize(name, port, &start_block)
    @name = name
    @port = port
    @start_block = start_block
  end
  
  def <=>(a)
    @name <=> a.name
  end
  
  def kill
    Process.kill('KILL', @pid)
  end
  
  def running?
    !@pid.nil?
  end
  
  def start
    puts "Starting #{name}"
    @pid = fork { @start_block.call }
  end
  
  def trial(options = {})
    concurrency = options[:concurrency] || 50
    size = options[:size] || 20 * 1.kilobyte
    requests = options[:requests] || 500
    
    print "#{@name} (c=#{concurrency},s=#{size})  "
    $stdout.flush
    r = %x{ab -t 3 -q -c #{concurrency} http://0.0.0.0:#{@port}/bytes/#{size}}
    # Complete requests:      1000

    return nil unless r =~ /Requests per second:\s*(\d+\.\d\d)/
    rps = $1.to_f
    if r =~ /Complete requests:\s*(\d+)/
      completed_requests = $1.to_i
    end
    puts "#{rps} req/sec (#{completed_requests} completed)"
    {
      :test => 'get',
      :server=> @name, 
      :concurrency => concurrency, 
      :size => size,
      :rps => rps,
      :requests => requests,
      :requests_completed => completed_requests,
      :time => Time.now
    }
  end
  
  
  def self.wait_scale
    [1,20,40,60,80,100]
  end
  def wait_trial(concurrency)
    
    print "#{@name} (c=#{concurrency})  "
    $stdout.flush
    r = %x{ab -t #{3} -q -c #{concurrency} http://0.0.0.0:#{@port}/periodical_activity/fibonacci/20}
    # Complete requests:      1000

    return nil unless r =~ /Requests per second:\s*(\d+\.\d\d)/
    rps = $1.to_f
    if r =~ /Complete requests:\s*(\d+)/
      completed_requests = $1.to_i
    end
    puts "#{rps} req/sec (#{completed_requests} completed)"
    {
      :test => 'get',
      :server=> @name, 
      :concurrency => concurrency,
      :rps => rps,
      :requests_completed => completed_requests,
      :time => Time.now
    }
  end
  
  
  def post_trial(size = 1, concurrency = 10)
    
    print "#{@name} (c=#{concurrency},posting=#{size})  "
    $stdout.flush
    
    fn = "/tmp/ebb_post_trial_#{size}"
    unless FileTest.exists?(fn)
      File.open(fn, 'w+') { |f| f.write("C"*size) }
    end
    
    r = %x{ab -t 6 -q -c #{concurrency} -p #{fn} http://0.0.0.0:#{@port}/test_post_length}
    
    return nil unless r =~ /Requests per second:\s*(\d+\.\d\d)/
    rps = $1.to_f
    if r =~ /Complete requests:\s*(\d+)/
      completed_requests = $1.to_i
    end
    puts "#{rps} req/sec (#{completed_requests} completed)"
    {
      :test => 'camping1',
      :server=> @name, 
      :concurrency => concurrency, 
      :size => size,
      :rps => rps,
      :requests_completed => completed_requests,
      :time => Time.now
    }
  end
  
end

$servers = []
app = SimpleApp.new
$servers << ServerTest.new('emongrel', 4001) do
  require 'mongrel'
  require 'swiftcore/evented_mongrel'
  ENV['EVENT'] = "1"
  Rack::Handler::Mongrel.run(app, :Port => 4001)
end

$servers << ServerTest.new('ebb', 4002) do
  require File.dirname(__FILE__) + '/../ruby_lib/ebb'
  server = Ebb::Server.new(app, :port => 4002)
  server.start
end

$servers << ServerTest.new('mongrel', 4003) do
 require 'mongrel'
 Rack::Handler::Mongrel.run(app, :Port => 4003)
end
 
$servers << ServerTest.new('thin', 4004) do
  require 'thin'
  Rack::Handler::Thin.run(app, :Port => 4004)
end


benchmark_type = ARGV.shift
servers_to_use = ARGV

trap('INT')  { exit(1) }
dumpfile = "#{benchmark_type}.dump"
begin
  results = ServerTestResults.open(dumpfile)
  $servers.each { |s| s.start }
  sleep 3
  ServerTest.send("#{benchmark_type}_scale").each do |s|
    $servers.rand_each do |server| 
      if r = server.send("#{benchmark_type}_trial", s)
        results << r
      else
        puts "error! restarting server"
        server.kill
        server.start
      end
      sleep 0.2 # give the other process some time to cool down?
    end
    puts "---"
  end
ensure
  puts "\n\nkilling servers"
  $servers.each { |server| server.kill }  
  results.write(dumpfile)
end