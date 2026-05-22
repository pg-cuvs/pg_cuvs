variable "project_id" {
  description = "GCP project ID"
  type        = string
}

variable "region" {
  description = "GCP region"
  type        = string
  default     = "asia-northeast3"
}

variable "zone" {
  description = "GCP zone (L4 availability)"
  type        = string
  default     = "asia-northeast3-b"
}

variable "instance_name" {
  description = "GPU VM instance name"
  type        = string
  default     = "pg-cuvs-dev"
}

variable "machine_type" {
  description = "Machine type — g2-standard-4 has 1x L4 + 4 vCPU + 16GB RAM"
  type        = string
  default     = "g2-standard-4"
}

variable "disk_size_gb" {
  description = "Boot disk size in GB (conda envs + PG data + index files)"
  type        = number
  default     = 100
}

variable "ssh_user" {
  description = "SSH username for Makefile gpu-* targets"
  type        = string
  default     = "ubuntu"
}

variable "ssh_pub_key_path" {
  description = "Path to SSH public key"
  type        = string
  default     = "~/.ssh/id_rsa.pub"
}

variable "preemptible" {
  description = "Use preemptible VM to reduce cost (stops after 24h)"
  type        = bool
  default     = false
}
