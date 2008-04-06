module Rack
  module Adapter
    autoload :Rails, Ebb::LIBDIR + '/rack/adapter/rails'
  end
end

module Ebb
  class Runner  
    class Rails < Runner
      def extra_options
        # defaults for ebb_rails
        @options.update(
          :environment => 'development',
          :port => 3000
        )

        @parser.on("-e", "--env ENV", 
                "Rails environment (default: development)") do |env| 
          @options[:environment] = env
        end
        @parser.on("-c", "--chdir DIR", "RAILS_ROOT directory") do |c| 
          @options[:root] = c
        end
      end

      def app(options)
        Rack::Adapter::Rails.new(options)
      end
    end
  end
end