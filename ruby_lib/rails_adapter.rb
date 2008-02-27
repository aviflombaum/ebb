# This as been submitted to Rack as a patch, tested and everything.
# Bug Christian Neukirchen at chneukirchen@gmail.com to apply the patch!

require 'cgi'
require 'rubygems'
require 'rack'

# Adapter to run a Rails app with any supported Rack handler.
# By default it will try to load the Rails application in the
# current directory in the development environment.
# Options:
#  root: Root directory of the Rails app
#  env: Rails environment to run in (development, production or test)
# Based on http://fuzed.rubyforge.org/ Rails adapter
class RailsAdapter
  def initialize(options={})
    @root = options[:root] || Dir.pwd
    @env  = options[:env]  || 'development'
    
    load_application
    
    @file_server = Rack::File.new(::File.join(RAILS_ROOT, "public"))
  end
  
  def load_application
    ENV['RAILS_ENV'] = @env

    require "#{@root}/config/environment"
    require 'dispatcher'
  end
  
  # TODO refactor this in File#can_serve?(path) ??
  def file?(path)
    full_path = ::File.join(@file_server.root, Utils.unescape(path))
    ::File.file?(full_path) && ::File.readable?(full_path)
  end
  
  def call(env)
    # Serve the file if it's there
    return @file_server.call(env) if file?(env['PATH_INFO'])
    
    request         = Request.new(env)
    response        = Response.new
    
    session_options = ActionController::CgiRequest::DEFAULT_SESSION_OPTIONS
    cgi             = CGIWrapper.new(request, response)

    Dispatcher.dispatch(cgi, session_options, response)

    response.finish
  end

  protected
    
    class CGIWrapper < ::CGI
      def initialize(request, response, *args)
        @request  = request
        @response = response
        @args     = *args
        @input    = request.body

        super *args
      end
    
      def header(options = "text/html")
        if options.is_a?(String)
          @response['Content-Type']     = options unless @response['Content-Type']
        else
          @response['Content-Length']   = options.delete('Content-Length').to_s if options['Content-Length']
        
          @response['Content-Type']     = options.delete('type') || "text/html"
          @response['Content-Type']    += "; charset=" + options.delete('charset') if options['charset']
                    
          @response['Content-Language'] = options.delete('language') if options['language']
          @response['Expires']          = options.delete('expires') if options['expires']

          @response.status              = options.delete('Status') if options['Status']
    
          options.each { |k,v| @response[k] = v }
        
          # Convert 'cookie' header to 'Set-Cookie' headers.
          # According to http://www.faqs.org/rfcs/rfc2109.html:
          #   the Set-Cookie response header comprises the token
          #   Set-Cookie:, followed by a comma-separated list of
          #   one or more cookies.
          if cookie = @response.header.delete('Cookie')
            cookies = case cookie
              when Array then cookie.collect { |c| c.to_s }.join(', ')
              when Hash  then cookie.collect { |_, c| c.to_s }.join(', ')
              else            cookie.to_s
            end
    
            cookies << ', ' + @output_cookies.each { |c| c.to_s }.join(', ') if @output_cookies
            
            @response['Set-Cookie'] = cookies
          end
        end
    
        ""
      end
                    
      def params
        @params ||= @request.params
      end
    
      def cookies
        @request.cookies
      end
    
      def query_string
        @request.query_string
      end
      
      # Used to wrap the normal args variable used inside CGI.
      def args
        @args
      end

      # Used to wrap the normal env_table variable used inside CGI.
      def env_table
        @request.env
      end

      # Used to wrap the normal stdinput variable used inside CGI.
      def stdinput
        @input
      end
    
      def stdoutput
        STDERR.puts "stdoutput should not be used."
        @response.body
      end
  end
end

