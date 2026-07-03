#!/usr/bin/env ruby
# ASCII waveform viewer for RTL trace files
# Usage: vcd.rb [options] <trace_file>
#   --vcd         Export to VCD format instead of ASCII waveform
#   --char-width  Characters per clock cycle (default: 5)

require 'optparse'

class TraceParser
  attr_reader :period, :signals, :timeline

  def initialize(filename)
    @filename = filename
    @period = nil
    @signals = {}  # signal_name => [timestamps where it changed]
    @timeline = []  # [{time:, changes: {signal => value}}]
    parse
  end

  def parse
    File.readlines(@filename).each do |line|
      line.strip!
      next if line.empty?

      if line.start_with?('header=>')
        parse_header(line)
      else
        parse_data(line)
      end
    end

    # Sort timeline by timestamp
    @timeline.sort_by! { |entry| entry[:time] }
  end

  private

  def parse_header(line)
    # Format: header=>period:10ns or header=>period: 10ns
    if line =~ /header=>period:\s*(\d+)(ns|us|ms|s)/
      @period = $1.to_i
      @period_unit = $2
    end
  end

  def parse_data(line)
    # Format: 5=>reset:1,count:0x0
    if line =~ /^(\d+)=>(.+)$/
      time = $1.to_i
      changes_str = $2

      changes = {}
      changes_str.split(',').each do |signal_value|
        if signal_value =~ /^([^:]+):(.+)$/
          signal_name = $1
          value = $2
          changes[signal_name] = value
          @signals[signal_name] ||= {}
        end
      end

      # Merge with existing timeline entry at same timestamp if it exists
      existing = @timeline.find { |entry| entry[:time] == time }
      if existing
        existing[:changes].merge!(changes)
      else
        @timeline << { time: time, changes: changes }
      end
    end
  end
end

class WaveformRenderer
  def initialize(parser, options = {})
    @parser = parser
    @scale = options[:scale] || 1  # characters per time unit
    @char_width = options[:char_width] || 10  # chars per clock cycle
  end

  def render
    return if @parser.timeline.empty?

    # Build full state for each signal over time
    signal_states = build_signal_states

    # Find time range
    min_time = @parser.timeline.first[:time]
    max_time = @parser.timeline.last[:time]

    # Print header
    puts "\nRTL Trace Waveform (period: #{@parser.period}ns)"
    puts "=" * 80
    puts

    # Print time ruler
    print_time_ruler(min_time, max_time)

    # Print clock signal first (derived from period)
    print_clock_waveform(min_time, max_time)

    # Print each signal
    signal_states.each do |signal_name, states|
      print_signal_waveform(signal_name, states, min_time, max_time)
    end

    puts
  end

  private

  def build_signal_states
    states = {}

    # Initialize all signals
    @parser.signals.keys.each do |sig|
      states[sig] = []
    end

    # Walk through timeline and build state changes (not full history)
    @parser.timeline.each do |entry|
      time = entry[:time]
      
      # Only record actual changes
      entry[:changes].each do |sig, val|
        states[sig] << { time: time, value: val }
      end
    end

    states
  end

  def print_time_ruler(min_time, max_time)
    period = @parser.period
    width = ((max_time - min_time) / period.to_f * @char_width).to_i

    # Print time markers
    print " " * 20  # offset for signal name
    (min_time..max_time).step(period * 2) do |t|
      pos = ((t - min_time) / period.to_f * @char_width).to_i
      print "#{t}ns".ljust(@char_width)
    end
    puts

    # Print ruler line
    print " " * 20
    puts "-" * width
  end

  def print_clock_waveform(min_time, max_time)
    period = @parser.period
    width = ((max_time - min_time) / period.to_f * @char_width).to_i + @char_width
    
    print "clk".ljust(20)
    
    output = Array.new(width, ' ')
    
    # Generate clock that toggles every half period
    half_period = period / 2.0
    current_val = 0  # Start low
    
    (min_time..max_time).step(period).each do |t|
      char_pos = ((t - min_time) / period.to_f * @char_width).to_i
      next if char_pos >= width
      
      # First half of cycle
      if current_val == 0
        # Rising edge
        output[char_pos] = '/'
        (char_pos + 1...char_pos + @char_width/2).each do |i|
          output[i] = '‾' if i < width
        end
        # Falling edge
        mid_pos = char_pos + @char_width/2
        output[mid_pos] = '\\' if mid_pos < width
        (mid_pos + 1...char_pos + @char_width).each do |i|
          output[i] = '_' if i < width
        end
      else
        # Continue from previous state
        (char_pos...char_pos + @char_width/2).each do |i|
          output[i] = '‾' if i < width
        end
        mid_pos = char_pos + @char_width/2
        output[mid_pos] = '\\' if mid_pos < width
        (mid_pos + 1...char_pos + @char_width).each do |i|
          output[i] = '_' if i < width
        end
      end
    end
    
    puts output.join
  end

  def print_signal_waveform(signal_name, states, min_time, max_time)
    period = @parser.period
    width = ((max_time - min_time) / period.to_f * @char_width).to_i + @char_width

    # Determine signal type (binary or multi-bit)
    is_binary = states.all? { |s| s[:value] == '0' || s[:value] == '1' }

    print signal_name.ljust(20)

    if is_binary
      render_binary_signal(states, min_time, max_time, width)
    else
      render_multibit_signal(states, min_time, max_time, width)
    end

    puts
  end

  def render_binary_signal(states, min_time, max_time, width)
    period = @parser.period
    output = Array.new(width, ' ')

    current_val = nil

    # Walk through each clock cycle
    (min_time..max_time).step(period).each do |t|
      # Find value at this time
      val = states.reverse.find { |s| s[:time] <= t }&.dig(:value)
      
      char_pos = ((t - min_time) / period.to_f * @char_width).to_i
      next if char_pos >= width

      if val != current_val && current_val != nil
        # Transition
        output[char_pos] = (val == '1' ? '/' : '\\')
        # Fill the rest of this segment with new level
        (char_pos + 1...char_pos + @char_width).each do |i|
          output[i] = (val == '1' ? '‾' : '_') if i < width
        end
        current_val = val
      elsif val != current_val
        # First value
        (char_pos...char_pos + @char_width).each do |i|
          output[i] = (val == '1' ? '‾' : '_') if i < width
        end
        current_val = val
      else
        # Continue current level
        (char_pos...char_pos + @char_width).each do |i|
          output[i] = (current_val == '1' ? '‾' : '_') if i < width && output[i] == ' '
        end
      end
    end

    print output.join
  end

  def render_multibit_signal(states, min_time, max_time, width)
    period = @parser.period
    output = Array.new(width, ' ')
    
    # Track what time each signal changes
    states.each_with_index do |state, idx|
      start_time = state[:time]
      end_time = states[idx + 1]&.dig(:time) || (max_time + period)
      
      start_pos = ((start_time - min_time) / period.to_f * @char_width).to_i
      end_pos = ((end_time - min_time) / period.to_f * @char_width).to_i
      
      next if start_pos >= width
      
      # Transition marker at start (except for first value)
      if idx > 0 && start_pos < width
        output[start_pos] = '|'
        start_pos += 1
      end
      
      # Calculate available space
      available_space = end_pos - start_pos
      val_str = "<#{state[:value]}>"
      
      # If not enough space for angle brackets, just show the value
      if available_space < val_str.length
        val_str = state[:value].to_s
      end
      
      # Center the value in the segment
      center_offset = [(available_space - val_str.length) / 2, 0].max
      value_pos = start_pos + center_offset
      
      # Fill with '=' before value
      (start_pos...value_pos).each do |i|
        break if i >= width
        output[i] = '=' if output[i] == ' '
      end
      
      # Place value (truncate if necessary)
      val_str.chars.each_with_index do |ch, i|
        pos = value_pos + i
        break if pos >= end_pos || pos >= width
        output[pos] = ch
      end
      
      # Fill with '=' after value until next transition
      (value_pos + val_str.length...end_pos).each do |i|
        break if i >= width
        output[i] = '=' if output[i] == ' '
      end
    end

    print output.join
  end
end

class VcdExporter
  def initialize(parser)
    @parser = parser
    @signal_ids = {}
    
    # Reserve ID for clock
    @signal_ids['clk'] = '!'
    
    # Assign short identifiers to each signal (", #, $, etc.)
    @parser.signals.keys.each_with_index do |sig, idx|
      @signal_ids[sig] = (34 + idx).chr  # Start from ASCII '"'
    end
  end

  def export
    puts "$date"
    puts "  #{Time.now}"
    puts "$end"
    puts "$version"
    puts "  RTL Trace to VCD Converter"
    puts "$end"
    puts "$timescale"
    puts "  1ns"
    puts "$end"
    puts "$scope module top $end"
    
    # Clock definition
    puts "$var wire 1 #{@signal_ids['clk']} clk $end"
    
    # Variable definitions
    @parser.signals.keys.each do |sig|
      # Determine wire width (assume 32 for hex values, 1 for binary)
      width = 1
      @parser.timeline.each do |entry|
        if entry[:changes][sig]
          val = entry[:changes][sig]
          if val.start_with?('0x')
            width = 32
            break
          end
        end
      end
      
      puts "$var wire #{width} #{@signal_ids[sig]} #{sig} $end"
    end
    
    puts "$upscope $end"
    puts "$enddefinitions $end"
    puts "$dumpvars"
    
    # Clock initial value
    puts "0#{@signal_ids['clk']}"
    
    # Initial values
    @parser.timeline.first[:changes].each do |sig, val|
      print_vcd_value(sig, val)
    end
    
    puts "$end"
    
    # Generate value changes with clock
    min_time = @parser.timeline.first[:time]
    max_time = @parser.timeline.last[:time]
    period = @parser.period
    half_period = period / 2
    
    # Track clock state
    clk_state = 0
    signal_changes = {}
    
    # Build map of time -> signal changes
    @parser.timeline.each do |entry|
      signal_changes[entry[:time]] = entry[:changes]
    end
    
    # Generate all time points (clock edges and signal changes)
    current_time = 0
    
    while current_time <= max_time + period
      # Output timestamp
      puts "##{current_time}"
      
      # Clock toggle every half period
      if current_time > 0 && current_time % half_period == 0
        clk_state = 1 - clk_state
        puts "#{clk_state}#{@signal_ids['clk']}"
      end
      
      # Signal changes at this time
      if signal_changes[current_time]
        signal_changes[current_time].each do |sig, val|
          print_vcd_value(sig, val)
        end
      end
      
      current_time += half_period
    end
  end

  private

  def print_vcd_value(signal, value)
    id = @signal_ids[signal]
    
    if value == '0' || value == '1'
      # Binary value
      puts "#{value}#{id}"
    elsif value.start_with?('0x')
      # Hex value - convert to binary
      hex_val = value[2..-1]
      bin_val = hex_val.to_i(16).to_s(2).rjust(32, '0')
      puts "b#{bin_val} #{id}"
    else
      # Treat as binary
      puts "b#{value} #{id}"
    end
  end
end

# Main execution
if __FILE__ == $0
  options = {
    vcd: false,
    char_width: 5
  }
  
  OptionParser.new do |opts|
    opts.banner = "Usage: #{$0} [options] <trace_file>"
    
    opts.on("--vcd", "Export to VCD format") do
      options[:vcd] = true
    end
    
    opts.on("--char-width WIDTH", Integer, "Characters per clock cycle (default: 5)") do |w|
      options[:char_width] = w
    end
    
    opts.on("-h", "--help", "Show this help") do
      puts opts
      exit
    end
  end.parse!
  
  if ARGV.empty?
    puts "Usage: #{$0} [options] <trace_file>"
    puts "Example: #{$0} counter.trace"
    puts "         #{$0} --vcd counter.trace > output.vcd"
    exit 1
  end

  trace_file = ARGV[0]
  
  unless File.exist?(trace_file)
    puts "Error: File '#{trace_file}' not found"
    exit 1
  end

  parser = TraceParser.new(trace_file)
  
  if options[:vcd]
    exporter = VcdExporter.new(parser)
    exporter.export
  else
    renderer = WaveformRenderer.new(parser, char_width: options[:char_width])
    renderer.render
  end
end
