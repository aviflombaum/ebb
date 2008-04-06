require 'optparse'

module Kernel
  unless respond_to? :daemonize # Already part of Ruby 1.9, yeah!
    # Turns the current script into a daemon process that detaches from the console.
    # It can be shut down with a TERM signal. Taken from ActiveSupport.
    def daemonize
      exit if fork                   # Parent exits, child continues.
      Process.setsid                 # Become session leader.
      exit if fork                   # Zap session leader. See [1].
      Dir.chdir "/"                  # Release old working directory.
      File.umask 0000                # Ensure sensible umask. Adjust as needed.
      STDIN.reopen "/dev/null"       # Free file descriptors and
      STDOUT.reopen "/dev/null", "a" # point them somewhere sensible.
      STDERR.reopen STDOUT           # STDOUT/ERR should better go to a logfile.
      trap("TERM") { exit }
    end
  end
end

module Ebb
  class Runner
    # Classes are modules and I hate this 'Base' class pattern. I'm putting
    # other classes inside this one.
    autoload :Rails, LIBDIR + '/ebb/runner/rails'
    
    # Kill the process which PID is stored in +pid_file+.
    def self.kill(pid_file, timeout=60)
      raise ArgumentError, 'You must specify a pid_file to stop deamonized server' unless pid_file 
      
      if pid = File.read(pid_file)
        pid = pid.to_i
        
        Process.kill('KILL', pid)
        Ebb.log.puts "stopped!"
      else
        Ebb.log.puts "Can't stop process, no PID found in #{@pid_file}"
      end
    rescue Errno::ESRCH # No such process
      Ebb.log.puts "process not found!"
    ensure
      File.delete(pid_file) rescue nil
    end

    def self.remove_pid_file(file)
      File.delete(file) if file && File.exists?(file) && Process.pid == File.read(file)
    end

    def self.write_pid_file(file)
      Ebb.log.puts ">> Writing PID to #{file}"
      open(file,"w+") { |f| f.write(Process.pid) }
      File.chmod(0644, file)
    end
    
    attr_reader :options
    
    def initialize
      @parser = OptionParser.new
      @options = {
        :port => 4001,
        :timeout => 60
      }
    end
    
    def parse_options(argv)
      @parser.banner = "Usage: #{self.class} [options] start | stop"
      @parser.separator ""
      extra_options if respond_to?(:extra_options)
      
      @parser.separator ""
      #  opts.on("-s", "--socket SOCKET", "listen on socket") { |socket| options[:socket] = socket }
      @parser.on("-p", "--port PORT", "(default: #{@options[:port]})") { |p| @options[:port]=p }
      @parser.on("-fd", "--fd FD", "listen on arbitrary fd") { |fd| @options[:fileno]=fd }
      @parser.on("-d", "--daemonize", "Daemonize") { @options[:daemonize] = true }
      @parser.on("-l", "--log-file FILE", "File to redirect output") { |f| @options[:log_file]=f }
      @parser.on("-P", "--pid-file FILE", "File to store PID") { |f| @options[:pid_file]=f }
      # @parser.on("-t", "--timeout SECONDS", "(default: #{@options[:timeout]})") { |s| @options[:timeout]=s }
      
      @parser.separator ""
      @parser.on_tail("-h", "--help", "Show this message") do
        Ebb.log.puts @parser
        exit
      end
      @parser.on_tail('-v', '--version', "Show version") do
        Ebb.log.puts Ebb::Client::BASE_ENV['SERVER_SOFTWARE']
        exit
      end
      
      @parser.parse!(argv)
    end
    
    def run(argv)
      parse_options(argv)
      
      case argv[0]
      when 'start'
        Ebb.log.print("Ebb is loading the application...")
        Ebb.log.flush()
        @app = app(@options)
        Ebb.log.puts("done")
        
        if @options[:daemonize]
          pwd = Dir.pwd # Current directory is changed during daemonization, so store it
          Kernel.daemonize 
          Dir.chdir pwd
          trap('HUP', 'IGNORE') # Don't die upon logout
        end
        
        if @options[:log_file]
          [STDOUT, STDERR].each { |f| f.reopen @options[:log_file], 'a' } 
        end
        
        if @options[:pid_file]
          Runner.write_pid_file(@options[:pid_file])
          at_exit do
            Ebb.log.puts ">> Exiting!"
            Runner.remove_pid_file(@options[:pid_file])
          end
        end
        
        Ebb::start_server(@app, @options)
      when 'stop'
        Ebb::Runner.kill @options[:pid_file], @options[:timeout]
      when nil
        Ebb.log.puts "Command required"
        Ebb.log.puts @parser
        exit 1
      else
        abort "Invalid command : #{argv[0]}"
      end
      
    end
  end
end

